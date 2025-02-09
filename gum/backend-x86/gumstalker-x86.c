/*
 * Copyright (C) 2009-2017 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 * Copyright (C) 2010-2013 Karl Trygve Kalleberg <karltk@boblycat.org>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#define ENABLE_DEBUG 0

#include "gumstalker.h"

#include "gummetalhash.h"
#include "gumx86reader.h"
#include "gumx86writer.h"
#include "gummemory.h"
#include "gumx86relocator.h"
#include "gumspinlock.h"
#include "gumtls.h"
#ifdef G_OS_WIN32
# include "gumexceptor.h"
#endif

#include <stdlib.h>
#include <string.h>
#ifdef G_OS_WIN32
# define VC_EXTRALEAN
# include <windows.h>
# include <psapi.h>
# include <tchar.h>
#endif

#define GUM_CODE_ALIGNMENT                     8
#define GUM_DATA_ALIGNMENT                     8
#define GUM_CODE_SLAB_SIZE_IN_PAGES         1024
#define GUM_EXEC_BLOCK_MIN_SIZE             2048

typedef struct _GumInfectContext GumInfectContext;
typedef struct _GumDisinfectContext GumDisinfectContext;

typedef struct _GumCallProbe GumCallProbe;
typedef struct _GumSlab GumSlab;

typedef struct _GumExecFrame GumExecFrame;
typedef struct _GumExecCtx GumExecCtx;
typedef void (* GumExecHelperWriteFunc) (GumExecCtx * ctx, GumX86Writer * cw);
typedef struct _GumExecBlock GumExecBlock;
typedef gpointer (GUM_THUNK * GumExecCtxReplaceCurrentBlockFunc) (
    GumExecCtx * ctx, gpointer start_address);

typedef guint GumPrologType;
typedef guint GumCodeContext;
typedef struct _GumGeneratorContext GumGeneratorContext;
typedef struct _GumCalloutEntry GumCalloutEntry;
typedef struct _GumInstruction GumInstruction;
typedef struct _GumBranchTarget GumBranchTarget;

typedef guint GumVirtualizationRequirements;

struct _GumStalkerPrivate
{
  guint page_size;

  GMutex mutex;
  GSList * contexts;
  GumTlsKey exec_ctx;

  GArray * exclusions;
  gint trust_threshold;
  volatile gboolean any_probes_attached;
  volatile gint last_probe_id;
  GumSpinlock probe_lock;
  GHashTable * probe_target_by_id;
  GHashTable * probe_array_by_address;

#ifdef G_OS_WIN32
  GumExceptor * exceptor;
  gpointer user32_start, user32_end;
  gpointer ki_user_callback_dispatcher_impl;
#endif
};

struct _GumInfectContext
{
  GumStalker * stalker;
  GumStalkerTransformer * transformer;
  GumEventSink * sink;
};

struct _GumDisinfectContext
{
  GumStalker * stalker;
  GumExecCtx * exec_ctx;
  gboolean success;
};

struct _GumCallProbe
{
  GumProbeId id;
  GumCallProbeCallback callback;
  gpointer user_data;
  GDestroyNotify user_notify;
};

struct _GumSlab
{
  guint8 * data;
  guint offset;
  guint size;
  GumSlab * next;
};

struct _GumExecFrame
{
  gpointer real_address;
  gpointer code_address;
};

enum _GumExecCtxState
{
  GUM_EXEC_CTX_ACTIVE,
  GUM_EXEC_CTX_UNFOLLOW_PENDING,
  GUM_EXEC_CTX_DESTROY_PENDING
};

struct _GumExecCtx
{
  volatile guint state;
  volatile gboolean invalidate_pending;

  GumStalker * stalker;
  GumThreadId thread_id;

  GumX86Writer code_writer;
  GumX86Relocator relocator;

  GumStalkerTransformer * transformer;
  GQueue callout_entries;
  GumSpinlock callout_lock;
  GumEventSink * sink;
  GumEventType sink_mask;
  void (* sink_process_impl) (GumEventSink * self, const GumEvent * ev);
  GumEvent tmp_event;

  gboolean unfollow_called_while_still_following;
  GumExecBlock * current_block;
  GumExecFrame * current_frame;
  GumExecFrame * first_frame;
  GumExecFrame * frames;

  gpointer resume_at;
  gpointer return_at;
  gpointer app_stack;

  gpointer thunks;
  gpointer infect_thunk;

  GumSlab * code_slab;
  GumSlab first_code_slab;
  gpointer last_prolog_minimal;
  gpointer last_epilog_minimal;
  gpointer last_prolog_full;
  gpointer last_epilog_full;
  gpointer last_stack_push;
  gpointer last_stack_pop_and_go;
  GumMetalHashTable * mappings;
};

struct _GumExecBlock
{
  GumExecCtx * ctx;
  GumSlab * slab;

  guint8 * real_begin;
  guint8 * real_end;
  guint8 * real_snapshot;
  guint8 * code_begin;
  guint8 * code_end;

  guint8 state;
  gint recycle_count;
  gboolean has_call_to_excluded_range;

#ifdef G_OS_WIN32
  DWORD previous_dr0;
  DWORD previous_dr1;
  DWORD previous_dr2;
  DWORD previous_dr7;
#endif
};

enum _GumExecState
{
  GUM_EXEC_NORMAL,
  GUM_EXEC_SINGLE_STEPPING_ON_CALL,
  GUM_EXEC_SINGLE_STEPPING_THROUGH_CALL
};

enum _GumPrologType
{
  GUM_PROLOG_NONE,
  GUM_PROLOG_MINIMAL,
  GUM_PROLOG_FULL,
  GUM_PROLOG_IC
};

enum _GumCodeContext
{
  GUM_CODE_INTERRUPTIBLE,
  GUM_CODE_UNINTERRUPTIBLE
};

struct _GumGeneratorContext
{
  GumInstruction * instruction;
  GumX86Relocator * relocator;
  GumX86Writer * code_writer;
  gpointer continuation_real_address;
  GumPrologType opened_prolog;
  guint accumulated_stack_delta;
};

struct _GumInstruction
{
  const cs_insn * ci;
  guint8 * begin;
  guint8 * end;
};

struct _GumStalkerIterator
{
  GumExecCtx * exec_context;
  GumExecBlock * exec_block;
  GumGeneratorContext * generator_context;

  GumInstruction instruction;
  GumVirtualizationRequirements requirements;
};

struct _GumCalloutEntry
{
  GumStalkerCallout callout;
  gpointer data;
  GDestroyNotify data_destroy;

  gpointer pc;

  GumExecCtx * exec_context;
};

struct _GumBranchTarget
{
  gpointer origin_ip;

  gpointer absolute_address;
  gssize relative_offset;

  gboolean is_indirect;
  uint8_t pfx_seg;
  x86_reg base;
  x86_reg index;
  guint8 scale;
};

enum _GumVirtualizationRequirements
{
  GUM_REQUIRE_NOTHING         = 0,

  GUM_REQUIRE_RELOCATION      = 1 << 0,
  GUM_REQUIRE_SINGLE_STEP     = 1 << 1
};

#define GUM_STALKER_LOCK(o) g_mutex_lock (&(o)->priv->mutex)
#define GUM_STALKER_UNLOCK(o) g_mutex_unlock (&(o)->priv->mutex)

#if GLIB_SIZEOF_VOID_P == 4
#define GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX (3)
#else
#define GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX (9)
#endif
#define GUM_MINIMAL_PROLOG_RETURN_OFFSET \
    ((GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX + 2) * sizeof (gpointer))
#define GUM_FULL_PROLOG_RETURN_OFFSET \
    (sizeof (GumCpuContext) + sizeof (gpointer))
#define GUM_THUNK_ARGLIST_STACK_RESERVE 64 /* x64 ABI compatibility */

static void gum_stalker_dispose (GObject * object);
static void gum_stalker_finalize (GObject * object);

G_GNUC_INTERNAL void _gum_stalker_do_follow_me (GumStalker * self,
    GumStalkerTransformer * transformer, GumEventSink * sink,
    volatile gpointer * ret_addr_ptr);
static void gum_stalker_infect (GumThreadId thread_id,
    GumCpuContext * cpu_context, gpointer user_data);
static void gum_stalker_disinfect (GumThreadId thread_id,
    GumCpuContext * cpu_context, gpointer user_data);

static void gum_stalker_free_probe_array (gpointer data);

static GumExecCtx * gum_stalker_create_exec_ctx (GumStalker * self,
    GumThreadId thread_id, GumStalkerTransformer * transformer,
    GumEventSink * sink);
static GumExecCtx * gum_stalker_get_exec_ctx (GumStalker * self);
static void gum_stalker_invalidate_caches (GumStalker * self);

static void gum_exec_ctx_dispose_callouts (GumExecCtx * ctx);
static void gum_exec_ctx_free (GumExecCtx * ctx);
static void gum_exec_ctx_unfollow (GumExecCtx * ctx, gpointer resume_at);
static gboolean gum_exec_ctx_has_executed (GumExecCtx * ctx);
static gpointer GUM_THUNK gum_exec_ctx_replace_current_block_with (
    GumExecCtx * ctx, gpointer start_address);
static void gum_exec_ctx_create_thunks (GumExecCtx * ctx);
static void gum_exec_ctx_destroy_thunks (GumExecCtx * ctx);

static GumExecBlock * gum_exec_ctx_obtain_block_for (GumExecCtx * ctx,
    gpointer real_address, gpointer * code_address);

static void gum_stalker_invoke_callout (GumCpuContext * cpu_context,
    GumCalloutEntry * entry);

static void gum_exec_ctx_write_prolog (GumExecCtx * ctx, GumPrologType type,
    GumX86Writer * cw);
static void gum_exec_ctx_write_epilog (GumExecCtx * ctx, GumPrologType type,
    GumX86Writer * cw);

static void gum_exec_ctx_ensure_inline_helpers_reachable (GumExecCtx * ctx);
static void gum_exec_ctx_write_minimal_prolog_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_minimal_epilog_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_full_prolog_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_full_epilog_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_prolog_helper (GumExecCtx * ctx,
    GumPrologType type, GumX86Writer * cw);
static void gum_exec_ctx_write_epilog_helper (GumExecCtx * ctx,
    GumPrologType type, GumX86Writer * cw);
static void gum_exec_ctx_write_stack_push_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_stack_pop_and_go_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_ensure_helper_reachable (GumExecCtx * ctx,
    gpointer * helper_ptr, GumExecHelperWriteFunc write);
static gboolean gum_exec_ctx_is_helper_reachable (GumExecCtx * ctx,
    gpointer * helper_ptr);

static void gum_exec_ctx_write_push_branch_target_address (GumExecCtx * ctx,
    const GumBranchTarget * target, GumGeneratorContext * gc);
static void gum_exec_ctx_load_real_register_into (GumExecCtx * ctx,
    GumCpuReg target_register, GumCpuReg source_register,
    gpointer ip, GumGeneratorContext * gc);
static void gum_exec_ctx_load_real_register_from_minimal_frame_into (
    GumExecCtx * ctx, GumCpuReg target_register, GumCpuReg source_register,
    gpointer ip, GumGeneratorContext * gc);
static void gum_exec_ctx_load_real_register_from_full_frame_into (
    GumExecCtx * ctx, GumCpuReg target_register, GumCpuReg source_register,
    gpointer ip, GumGeneratorContext * gc);
static void gum_exec_ctx_load_real_register_from_ic_frame_into (
    GumExecCtx * ctx, GumCpuReg target_register, GumCpuReg source_register,
    gpointer ip, GumGeneratorContext * gc);

static GumExecBlock * gum_exec_block_new (GumExecCtx * ctx);
static GumExecBlock * gum_exec_block_obtain (GumExecCtx * ctx,
    gpointer real_address, gpointer * code_address);
static gboolean gum_exec_block_is_full (GumExecBlock * block);
static void gum_exec_block_commit (GumExecBlock * block);

static void gum_exec_block_backpatch_call (GumExecBlock * block,
    gpointer code_start, GumPrologType opened_prolog, gpointer ret_real_address,
    gpointer ret_code_address);
static void gum_exec_block_backpatch_jmp (GumExecBlock * block,
    gpointer code_start, GumPrologType opened_prolog);
static void gum_exec_block_backpatch_ret (GumExecBlock * block,
    gpointer code_start);

static GumVirtualizationRequirements gum_exec_block_virtualize_branch_insn (
    GumExecBlock * block, GumGeneratorContext * gc);
static GumVirtualizationRequirements gum_exec_block_virtualize_ret_insn (
    GumExecBlock * block, GumGeneratorContext * gc);
static GumVirtualizationRequirements gum_exec_block_virtualize_sysenter_insn (
    GumExecBlock * block, GumGeneratorContext * gc);

static void gum_exec_block_write_call_invoke_code (GumExecBlock * block,
    const GumBranchTarget * target, GumGeneratorContext * gc);
static void gum_exec_block_write_jmp_transfer_code (GumExecBlock * block,
    const GumBranchTarget * target, GumExecCtxReplaceCurrentBlockFunc func,
    GumGeneratorContext * gc);
static void gum_exec_block_write_ret_transfer_code (GumExecBlock * block,
    GumGeneratorContext * gc);
static void gum_exec_block_write_single_step_transfer_code (
    GumExecBlock * block, GumGeneratorContext * gc);

static void gum_exec_block_write_call_event_code (GumExecBlock * block,
    const GumBranchTarget * target, GumGeneratorContext * gc,
    GumCodeContext cc);
static void gum_exec_block_write_ret_event_code (GumExecBlock * block,
    GumGeneratorContext * gc, GumCodeContext cc);
static void gum_exec_block_write_exec_event_code (GumExecBlock * block,
    GumGeneratorContext * gc, GumCodeContext cc);
static void gum_exec_block_write_block_event_code (GumExecBlock * block,
    GumGeneratorContext * gc, GumCodeContext cc);
static void gum_exec_block_write_unfollow_check_code (GumExecBlock * block,
    GumGeneratorContext * gc, GumCodeContext cc);

static void gum_exec_block_write_call_probe_code (GumExecBlock * block,
    const GumBranchTarget * target, GumGeneratorContext * gc);

static void gum_exec_block_open_prolog (GumExecBlock * block,
    GumPrologType type, GumGeneratorContext * gc);
static void gum_exec_block_close_prolog (GumExecBlock * block,
    GumGeneratorContext * gc);

static void gum_write_segment_prefix (uint8_t segment, GumX86Writer * cw);

static GumCpuReg gum_cpu_meta_reg_from_real_reg (GumCpuReg reg);
static GumCpuReg gum_cpu_reg_from_capstone (x86_reg reg);
static x86_insn gum_negate_jcc (x86_insn instruction_id);

#ifdef G_OS_WIN32
static gboolean gum_stalker_on_exception (GumExceptionDetails * details,
    gpointer user_data);
#endif

G_DEFINE_TYPE (GumStalker, gum_stalker, G_TYPE_OBJECT);

static void
gum_stalker_class_init (GumStalkerClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GumStalkerPrivate));

  object_class->dispose = gum_stalker_dispose;
  object_class->finalize = gum_stalker_finalize;
}

static void
gum_stalker_init (GumStalker * self)
{
  GumStalkerPrivate * priv;

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GUM_TYPE_STALKER, GumStalkerPrivate);
  priv = self->priv;

  priv->exclusions = g_array_new (FALSE, FALSE, sizeof (GumMemoryRange));
  priv->trust_threshold = 1;

  gum_spinlock_init (&priv->probe_lock);
  priv->probe_target_by_id =
      g_hash_table_new_full (NULL, NULL, NULL, NULL);
  priv->probe_array_by_address =
      g_hash_table_new_full (NULL, NULL, NULL, gum_stalker_free_probe_array);

#if defined (G_OS_WIN32) && GLIB_SIZEOF_VOID_P == 4
  priv->exceptor = gum_exceptor_obtain ();
  gum_exceptor_add (priv->exceptor, gum_stalker_on_exception, self);

  {
    HMODULE ntmod, usermod;
    MODULEINFO mi;
    BOOL success;
    gboolean found_user32_code = FALSE;
    guint8 * p;

    ntmod = GetModuleHandle (_T ("ntdll.dll"));
    usermod = GetModuleHandle (_T ("user32.dll"));
    g_assert (ntmod != NULL && usermod != NULL);

    success = GetModuleInformation (GetCurrentProcess (), usermod,
        &mi, sizeof (mi));
    g_assert (success);
    priv->user32_start = mi.lpBaseOfDll;
    priv->user32_end = (guint8 *) mi.lpBaseOfDll + mi.SizeOfImage;

    for (p = (guint8 *) priv->user32_start; p < (guint8 *) priv->user32_end;)
    {
      MEMORY_BASIC_INFORMATION mbi;

      success = VirtualQuery (p, &mbi, sizeof (mbi)) == sizeof (mbi);
      g_assert (success);

      if (mbi.Protect == PAGE_EXECUTE_READ ||
          mbi.Protect == PAGE_EXECUTE_READWRITE ||
          mbi.Protect == PAGE_EXECUTE_WRITECOPY)
      {
        priv->user32_start = mbi.BaseAddress;
        priv->user32_end = (guint8 *) mbi.BaseAddress + mbi.RegionSize;

        found_user32_code = TRUE;
      }

      p = (guint8 *) mbi.BaseAddress + mbi.RegionSize;
    }

    g_assert (found_user32_code);

    priv->ki_user_callback_dispatcher_impl = GUM_FUNCPTR_TO_POINTER (
        GetProcAddress (ntmod, "KiUserCallbackDispatcher"));
    g_assert (priv->ki_user_callback_dispatcher_impl != NULL);
  }
#endif

  priv->page_size = gum_query_page_size ();
  g_mutex_init (&priv->mutex);
  priv->contexts = NULL;
  priv->exec_ctx = gum_tls_key_new ();
}

static void
gum_stalker_dispose (GObject * object)
{
#if defined (G_OS_WIN32) && GLIB_SIZEOF_VOID_P == 4
  GumStalker * self = GUM_STALKER (object);
  GumStalkerPrivate * priv = self->priv;

  if (priv->exceptor != NULL)
  {
    gum_exceptor_remove (priv->exceptor, gum_stalker_on_exception, self);
    g_object_unref (priv->exceptor);
    priv->exceptor = NULL;
  }
#endif

  G_OBJECT_CLASS (gum_stalker_parent_class)->dispose (object);
}

static void
gum_stalker_finalize (GObject * object)
{
  GumStalker * self = GUM_STALKER (object);
  GumStalkerPrivate * priv = self->priv;

  g_hash_table_unref (priv->probe_array_by_address);
  g_hash_table_unref (priv->probe_target_by_id);

  gum_spinlock_free (&priv->probe_lock);

  g_array_free (priv->exclusions, TRUE);

  g_assert (priv->contexts == NULL);
  gum_tls_key_free (priv->exec_ctx);
  g_mutex_clear (&priv->mutex);

  G_OBJECT_CLASS (gum_stalker_parent_class)->finalize (object);
}

GumStalker *
gum_stalker_new (void)
{
  return GUM_STALKER (g_object_new (GUM_TYPE_STALKER, NULL));
}

void
gum_stalker_exclude (GumStalker * self,
                     const GumMemoryRange * range)
{
  g_array_append_val (self->priv->exclusions, *range);
}

gint
gum_stalker_get_trust_threshold (GumStalker * self)
{
  return self->priv->trust_threshold;
}

void
gum_stalker_set_trust_threshold (GumStalker * self,
                                 gint trust_threshold)
{
  self->priv->trust_threshold = trust_threshold;
}

void
gum_stalker_stop (GumStalker * self)
{
  GumStalkerPrivate * priv = self->priv;
  gboolean rescan_needed;
  GSList * cur;

  gum_spinlock_acquire (&priv->probe_lock);
  g_hash_table_remove_all (priv->probe_target_by_id);
  g_hash_table_remove_all (priv->probe_array_by_address);
  priv->any_probes_attached = FALSE;
  gum_spinlock_release (&priv->probe_lock);

  GUM_STALKER_LOCK (self);

  do
  {
    rescan_needed = FALSE;

    for (cur = priv->contexts; cur != NULL; cur = cur->next)
    {
      GumExecCtx * ctx = (GumExecCtx *) cur->data;
      if (ctx->state == GUM_EXEC_CTX_ACTIVE)
      {
        GumThreadId thread_id = ctx->thread_id;

        GUM_STALKER_UNLOCK (self);
        gum_stalker_unfollow (self, thread_id);
        GUM_STALKER_LOCK (self);

        rescan_needed = TRUE;
        break;
      }
    }
  }
  while (rescan_needed);

  GUM_STALKER_UNLOCK (self);

  gum_stalker_garbage_collect (self);
}

gboolean
gum_stalker_garbage_collect (GumStalker * self)
{
  GSList * keep = NULL, * cur;
  gboolean pending_garbage;

  GUM_STALKER_LOCK (self);

  for (cur = self->priv->contexts; cur != NULL; cur = cur->next)
  {
    GumExecCtx * ctx = (GumExecCtx *) cur->data;
    if (ctx->state == GUM_EXEC_CTX_DESTROY_PENDING)
      gum_exec_ctx_free (ctx);
    else
      keep = g_slist_prepend (keep, ctx);
  }

  g_slist_free (self->priv->contexts);
  self->priv->contexts = keep;

  pending_garbage = keep != NULL;

  GUM_STALKER_UNLOCK (self);

  return pending_garbage;
}

#ifdef _MSC_VER

#define RETURN_ADDRESS_POINTER_FROM_FIRST_ARGUMENT(arg)   \
    ((gpointer *) ((volatile guint8 *) &arg - sizeof (gpointer)))

void
gum_stalker_follow_me (GumStalker * self,
                       GumStalkerTransformer * transformer,
                       GumEventSink * sink)
{
  volatile gpointer * ret_addr_ptr;

  ret_addr_ptr = RETURN_ADDRESS_POINTER_FROM_FIRST_ARGUMENT (self);

  _gum_stalker_do_follow_me (self, transformer, sink, ret_addr_ptr);
}

#endif

void
_gum_stalker_do_follow_me (GumStalker * self,
                           GumStalkerTransformer * transformer,
                           GumEventSink * sink,
                           volatile gpointer * ret_addr_ptr)
{
  GumExecCtx * ctx;
  gpointer code_address;

  ctx = gum_stalker_create_exec_ctx (self, gum_process_get_current_thread_id (),
      transformer, sink);
  gum_tls_key_set_value (self->priv->exec_ctx, ctx);
  ctx->current_block = gum_exec_ctx_obtain_block_for (ctx, *ret_addr_ptr,
      &code_address);
  *ret_addr_ptr = code_address;

  gum_event_sink_start (sink);
}

void
gum_stalker_unfollow_me (GumStalker * self)
{
  GumExecCtx * ctx;

  ctx = gum_stalker_get_exec_ctx (self);
  g_assert (ctx != NULL);

  gum_exec_ctx_dispose_callouts (ctx);

  gum_event_sink_stop (ctx->sink);

  if (ctx->current_block != NULL &&
      ctx->current_block->has_call_to_excluded_range)
  {
    ctx->state = GUM_EXEC_CTX_UNFOLLOW_PENDING;
  }
  else
  {
    g_assert (ctx->unfollow_called_while_still_following);

    gum_tls_key_set_value (self->priv->exec_ctx, NULL);

    GUM_STALKER_LOCK (self);
    self->priv->contexts = g_slist_remove (self->priv->contexts, ctx);
    GUM_STALKER_UNLOCK (self);

    gum_exec_ctx_free (ctx);
  }
}

gboolean
gum_stalker_is_following_me (GumStalker * self)
{
  return gum_stalker_get_exec_ctx (self) != NULL;
}

void
gum_stalker_follow (GumStalker * self,
                    GumThreadId thread_id,
                    GumStalkerTransformer * transformer,
                    GumEventSink * sink)
{
  if (thread_id == gum_process_get_current_thread_id ())
  {
    gum_stalker_follow_me (self, transformer, sink);
  }
  else
  {
    GumInfectContext ctx;
    ctx.stalker = self;
    ctx.transformer = transformer;
    ctx.sink = sink;
    gum_process_modify_thread (thread_id, gum_stalker_infect, &ctx);
  }
}

void
gum_stalker_unfollow (GumStalker * self,
                      GumThreadId thread_id)
{
  if (thread_id == gum_process_get_current_thread_id ())
  {
    gum_stalker_unfollow_me (self);
  }
  else
  {
    GSList * cur;

    GUM_STALKER_LOCK (self);

    for (cur = self->priv->contexts; cur != NULL; cur = cur->next)
    {
      GumExecCtx * ctx = (GumExecCtx *) cur->data;
      if (ctx->thread_id == thread_id && ctx->state == GUM_EXEC_CTX_ACTIVE)
      {
        gum_exec_ctx_dispose_callouts (ctx);

        gum_event_sink_stop (ctx->sink);

        if (gum_exec_ctx_has_executed (ctx))
        {
          ctx->state = GUM_EXEC_CTX_UNFOLLOW_PENDING;
        }
        else
        {
          GumDisinfectContext dc;
          dc.stalker = self;
          dc.exec_ctx = ctx;
          dc.success = FALSE;
          gum_process_modify_thread (thread_id, gum_stalker_disinfect, &dc);
          if (!dc.success)
            ctx->state = GUM_EXEC_CTX_UNFOLLOW_PENDING;
        }

        break;
      }
    }

    GUM_STALKER_UNLOCK (self);
  }
}

static void
gum_stalker_infect (GumThreadId thread_id,
                    GumCpuContext * cpu_context,
                    gpointer user_data)
{
  GumInfectContext * infect_context = (GumInfectContext *) user_data;
  GumStalker * self = infect_context->stalker;
  GumExecCtx * ctx;
  gpointer code_address;
  GumX86Writer cw;

  ctx = gum_stalker_create_exec_ctx (self, thread_id,
      infect_context->transformer, infect_context->sink);

  ctx->current_block = gum_exec_ctx_obtain_block_for (ctx,
      GSIZE_TO_POINTER (GUM_CPU_CONTEXT_XIP (cpu_context)), &code_address);
  GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (ctx->infect_thunk);

  gum_x86_writer_init (&cw, ctx->infect_thunk);
  gum_exec_ctx_write_prolog (ctx, GUM_PROLOG_MINIMAL, &cw);
  gum_x86_writer_put_call_address_with_aligned_arguments (&cw, GUM_CALL_CAPI,
      GUM_ADDRESS (gum_tls_key_set_value), 2,
      GUM_ARG_ADDRESS, GUM_ADDRESS (self->priv->exec_ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (ctx));
  gum_exec_ctx_write_epilog (ctx, GUM_PROLOG_MINIMAL, &cw);
  gum_x86_writer_put_jmp_address (&cw, GUM_ADDRESS (code_address));
  gum_x86_writer_clear (&cw);

  gum_event_sink_start (infect_context->sink);
}

static void
gum_stalker_disinfect (GumThreadId thread_id,
                       GumCpuContext * cpu_context,
                       gpointer user_data)
{
  GumDisinfectContext * disinfect_context = (GumDisinfectContext *) user_data;
  GumStalker * self = disinfect_context->stalker;
  GumExecCtx * ctx = disinfect_context->exec_ctx;
  gboolean infection_not_active_yet;

  (void) thread_id;

  infection_not_active_yet =
      GUM_CPU_CONTEXT_XIP (cpu_context) == GPOINTER_TO_SIZE (ctx->infect_thunk);
  if (infection_not_active_yet)
  {
    GUM_CPU_CONTEXT_XIP (cpu_context) =
        GPOINTER_TO_SIZE (ctx->current_block->real_begin);

    self->priv->contexts = g_slist_remove (self->priv->contexts, ctx);
    gum_exec_ctx_free (ctx);

    disinfect_context->success = TRUE;
  }
}

GumProbeId
gum_stalker_add_call_probe (GumStalker * self,
                            gpointer target_address,
                            GumCallProbeCallback callback,
                            gpointer data,
                            GDestroyNotify notify)
{
  GumStalkerPrivate * priv = self->priv;
  GumCallProbe probe;
  GArray * probes;

  probe.id = g_atomic_int_add (&priv->last_probe_id, 1) + 1;
  probe.callback = callback;
  probe.user_data = data;
  probe.user_notify = notify;

  gum_spinlock_acquire (&priv->probe_lock);

  g_hash_table_insert (priv->probe_target_by_id, GSIZE_TO_POINTER (probe.id),
      target_address);

  probes = (GArray *)
      g_hash_table_lookup (priv->probe_array_by_address, target_address);
  if (probes == NULL)
  {
    probes = g_array_sized_new (FALSE, FALSE, sizeof (GumCallProbe), 4);
    g_hash_table_insert (priv->probe_array_by_address, target_address, probes);
  }

  g_array_append_val (probes, probe);

  priv->any_probes_attached = TRUE;

  gum_spinlock_release (&priv->probe_lock);

  gum_stalker_invalidate_caches (self);

  return probe.id;
}

void
gum_stalker_remove_call_probe (GumStalker * self,
                               GumProbeId id)
{
  GumStalkerPrivate * priv = self->priv;
  gpointer target_address;

  gum_spinlock_acquire (&priv->probe_lock);

  target_address =
      g_hash_table_lookup (priv->probe_target_by_id, GSIZE_TO_POINTER (id));
  if (target_address != NULL)
  {
    GArray * probes;
    gint match_index = -1;
    guint i;
    GumCallProbe * probe;

    g_hash_table_remove (priv->probe_target_by_id, GSIZE_TO_POINTER (id));

    probes = (GArray *)
        g_hash_table_lookup (priv->probe_array_by_address, target_address);
    g_assert (probes != NULL);

    for (i = 0; i != probes->len; i++)
    {
      if (g_array_index (probes, GumCallProbe, i).id == id)
      {
        match_index = i;
        break;
      }
    }
    g_assert_cmpint (match_index, !=, -1);

    probe = &g_array_index (probes, GumCallProbe, match_index);
    if (probe->user_notify != NULL)
      probe->user_notify (probe->user_data);
    g_array_remove_index (probes, match_index);

    if (probes->len == 0)
      g_hash_table_remove (priv->probe_array_by_address, target_address);

    priv->any_probes_attached =
        g_hash_table_size (priv->probe_array_by_address) != 0;
  }

  gum_spinlock_release (&priv->probe_lock);

  gum_stalker_invalidate_caches (self);
}

static void
gum_stalker_free_probe_array (gpointer data)
{
  GArray * probes = (GArray *) data;
  guint i;

  for (i = 0; i != probes->len; i++)
  {
    GumCallProbe * probe = &g_array_index (probes, GumCallProbe, i);
    if (probe->user_notify != NULL)
      probe->user_notify (probe->user_data);
  }

  g_array_free (probes, TRUE);
}

static GumExecCtx *
gum_stalker_create_exec_ctx (GumStalker * self,
                             GumThreadId thread_id,
                             GumStalkerTransformer * transformer,
                             GumEventSink * sink)
{
  GumStalkerPrivate * priv = self->priv;
  guint base_size;
  GumExecCtx * ctx;

  base_size = sizeof (GumExecCtx) / priv->page_size;
  if (sizeof (GumExecCtx) % priv->page_size != 0)
    base_size++;

  ctx = (GumExecCtx *)
      gum_alloc_n_pages (base_size + GUM_CODE_SLAB_SIZE_IN_PAGES + 1,
          GUM_PAGE_RWX);
  ctx->state = GUM_EXEC_CTX_ACTIVE;
  ctx->invalidate_pending = FALSE;

  ctx->code_slab = &ctx->first_code_slab;
  ctx->first_code_slab.data = ((guint8 *) ctx) + (base_size * priv->page_size);
  ctx->first_code_slab.offset = 0;
  ctx->first_code_slab.size = GUM_CODE_SLAB_SIZE_IN_PAGES * priv->page_size;
  ctx->first_code_slab.next = NULL;
  ctx->last_prolog_minimal = NULL;
  ctx->last_epilog_minimal = NULL;
  ctx->last_prolog_full = NULL;
  ctx->last_epilog_full = NULL;
  ctx->last_stack_push = NULL;
  ctx->last_stack_pop_and_go = NULL;

  ctx->frames = (GumExecFrame *) (ctx->code_slab->data + ctx->code_slab->size);
  ctx->first_frame = (GumExecFrame *) (ctx->code_slab->data +
      ctx->code_slab->size + priv->page_size - sizeof (GumExecFrame));
  ctx->current_frame = ctx->first_frame;

  ctx->mappings = gum_metal_hash_table_new (NULL, NULL);

  ctx->resume_at = NULL;
  ctx->return_at = NULL;
  ctx->app_stack = NULL;

  ctx->stalker = g_object_ref (self);
  ctx->thread_id = thread_id;

  gum_x86_writer_init (&ctx->code_writer, NULL);
  gum_x86_relocator_init (&ctx->relocator, NULL, &ctx->code_writer);

  if (transformer != NULL)
    ctx->transformer = g_object_ref (transformer);
  else
    ctx->transformer = gum_stalker_transformer_make_default ();
  g_queue_init (&ctx->callout_entries);
  gum_spinlock_init (&ctx->callout_lock);
  ctx->sink = (GumEventSink *) g_object_ref (sink);
  ctx->sink_mask = gum_event_sink_query_mask (sink);
  ctx->sink_process_impl = GUM_EVENT_SINK_GET_INTERFACE (sink)->process;

  gum_exec_ctx_create_thunks (ctx);

  GUM_STALKER_LOCK (self);
  self->priv->contexts = g_slist_prepend (self->priv->contexts, ctx);
  GUM_STALKER_UNLOCK (self);

  gum_exec_ctx_ensure_inline_helpers_reachable (ctx);

  return ctx;
}

static GumExecCtx *
gum_stalker_get_exec_ctx (GumStalker * self)
{
  return (GumExecCtx *) gum_tls_key_get_value (self->priv->exec_ctx);
}

static void
gum_stalker_invalidate_caches (GumStalker * self)
{
  GSList * cur;

  GUM_STALKER_LOCK (self);

  for (cur = self->priv->contexts; cur != NULL; cur = cur->next)
  {
    GumExecCtx * ctx = (GumExecCtx *) cur->data;

    ctx->invalidate_pending = TRUE;
  }

  GUM_STALKER_UNLOCK (self);
}

static void
gum_exec_ctx_dispose_callouts (GumExecCtx * ctx)
{
  GList * cur;

  gum_spinlock_acquire (&ctx->callout_lock);

  for (cur = ctx->callout_entries.head; cur != NULL; cur = cur->next)
  {
    GumCalloutEntry * entry = cur->data;

    if (entry->data_destroy != NULL)
      entry->data_destroy (entry->data);

    entry->callout = NULL;
    entry->data = NULL;
    entry->data_destroy = NULL;
  }

  gum_spinlock_release (&ctx->callout_lock);
}

static void
gum_exec_ctx_finalize_callouts (GumExecCtx * ctx)
{
  GList * cur;

  for (cur = ctx->callout_entries.head; cur != NULL; cur = cur->next)
  {
    GumCalloutEntry * entry = cur->data;

    g_slice_free (GumCalloutEntry, entry);
  }

  g_queue_clear (&ctx->callout_entries);
}

static void
gum_exec_ctx_free (GumExecCtx * ctx)
{
  GumSlab * slab;

  gum_metal_hash_table_unref (ctx->mappings);

  slab = ctx->code_slab;
  while (slab != &ctx->first_code_slab)
  {
    GumSlab * next = slab->next;
    gum_free_pages (slab);
    slab = next;
  }

  gum_exec_ctx_destroy_thunks (ctx);

  g_object_unref (ctx->sink);
  gum_exec_ctx_finalize_callouts (ctx);
  gum_spinlock_free (&ctx->callout_lock);
  g_object_unref (ctx->transformer);

  gum_x86_relocator_clear (&ctx->relocator);
  gum_x86_writer_clear (&ctx->code_writer);

  g_object_unref (ctx->stalker);

  gum_free_pages (ctx);
}

static void
gum_exec_ctx_unfollow (GumExecCtx * ctx,
                       gpointer resume_at)
{
  ctx->resume_at = resume_at;

  gum_tls_key_set_value (ctx->stalker->priv->exec_ctx, NULL);
  ctx->current_block = NULL;
  ctx->state = GUM_EXEC_CTX_DESTROY_PENDING;
}

static gboolean
gum_exec_ctx_has_executed (GumExecCtx * ctx)
{
  return ctx->resume_at != NULL;
}

static gboolean counters_enabled = FALSE;
static guint total_transitions = 0;

#define GUM_ENTRYGATE(name) \
  gum_exec_ctx_replace_current_block_from_##name
#define GUM_DEFINE_ENTRYGATE(name) \
  static guint total_##name##s = 0; \
  \
  static gpointer GUM_THUNK \
  GUM_ENTRYGATE (name) ( \
      GumExecCtx * ctx, \
      gpointer start_address) \
  { \
    if (counters_enabled) \
      total_##name##s++; \
    \
    return gum_exec_ctx_replace_current_block_with (ctx, start_address); \
  }
#define GUM_PRINT_ENTRYGATE_COUNTER(name) \
  g_printerr ("\t" G_STRINGIFY (name) "s: %u\n", total_##name##s)

#if GLIB_SIZEOF_VOID_P == 4 && !defined (HAVE_QNX)
GUM_DEFINE_ENTRYGATE (sysenter_slow_path)
#endif

GUM_DEFINE_ENTRYGATE (call_imm)
GUM_DEFINE_ENTRYGATE (call_reg)
GUM_DEFINE_ENTRYGATE (call_mem)
GUM_DEFINE_ENTRYGATE (post_call_invoke)
GUM_DEFINE_ENTRYGATE (ret_slow_path)

GUM_DEFINE_ENTRYGATE (jmp_imm)
GUM_DEFINE_ENTRYGATE (jmp_mem)
GUM_DEFINE_ENTRYGATE (jmp_reg)

GUM_DEFINE_ENTRYGATE (jmp_cond_imm)
GUM_DEFINE_ENTRYGATE (jmp_cond_mem)
GUM_DEFINE_ENTRYGATE (jmp_cond_reg)
GUM_DEFINE_ENTRYGATE (jmp_cond_jcxz)

GUM_DEFINE_ENTRYGATE (jmp_continuation)

static gpointer GUM_THUNK
gum_exec_ctx_replace_current_block_with (GumExecCtx * ctx,
                                         gpointer start_address)
{
  if (counters_enabled)
    total_transitions++;

  if (ctx->invalidate_pending)
  {
    gum_metal_hash_table_remove_all (ctx->mappings);

    ctx->invalidate_pending = FALSE;
  }

  if (start_address == gum_stalker_unfollow_me)
  {
    ctx->unfollow_called_while_still_following = TRUE;
    ctx->current_block = NULL;
    ctx->resume_at = start_address;
  }
  else if (ctx->state == GUM_EXEC_CTX_UNFOLLOW_PENDING)
  {
    gum_exec_ctx_unfollow (ctx, start_address);
  }
  else
  {
    ctx->current_block = gum_exec_ctx_obtain_block_for (ctx, start_address,
        &ctx->resume_at);
  }

  return ctx->resume_at;
}

static void
gum_exec_ctx_create_thunks (GumExecCtx * ctx)
{
  GumX86Writer cw;

  g_assert (ctx->thunks == NULL);

  ctx->thunks = gum_alloc_n_pages (1, GUM_PAGE_RWX);
  gum_x86_writer_init (&cw, ctx->thunks);

  ctx->infect_thunk = gum_x86_writer_cur (&cw);

  gum_x86_writer_clear (&cw);
}

static void
gum_exec_ctx_destroy_thunks (GumExecCtx * ctx)
{
  gum_free_pages (ctx->thunks);
}

#if ENABLE_DEBUG

static void
gum_disasm (guint8 * code,
            guint size,
            const gchar * prefix)
{
  csh capstone;
  cs_err err;
  cs_insn * insn;
  size_t count, i;

  err = cs_open (CS_ARCH_X86, GUM_CPU_MODE, &capstone);
  g_assert_cmpint (err, == , CS_ERR_OK);

  count = cs_disasm (capstone, code, size, GPOINTER_TO_SIZE (code), 0, &insn);
  g_assert (insn != NULL);

  for (i = 0; i != count; i++)
  {
    printf ("%s0x%" G_GINT64_MODIFIER "x\t%s %s\n",
        prefix, insn[i].address, insn[i].mnemonic, insn[i].op_str);
  }

  cs_free (insn, count);

  cs_close (&capstone);
}

static void
gum_hexdump (guint8 * data, guint size, const gchar * prefix)
{
  guint i, line_offset;

  line_offset = 0;
  for (i = 0; i != size; i++)
  {
    if (line_offset == 0)
      printf ("%s0x%" G_GINT64_MODIFIER "x\t%02x",
          prefix, (guint64) GPOINTER_TO_SIZE (data + i), data[i]);
    else
      printf (" %02x", data[i]);

    line_offset++;
    if (line_offset == 16 && i != size - 1)
    {
      printf ("\n");
      line_offset = 0;
    }
  }

  if (line_offset != 0)
    printf ("\n");
}

#endif

static GumExecBlock *
gum_exec_ctx_obtain_block_for (GumExecCtx * ctx,
                               gpointer real_address,
                               gpointer * code_address)
{
  GumExecBlock * block;
  GumX86Writer * cw;
  GumX86Relocator * rl;
  GumGeneratorContext gc;
  GumStalkerIterator iterator;
  gboolean all_labels_resolved;

  if (ctx->stalker->priv->trust_threshold >= 0)
  {
    block = gum_exec_block_obtain (ctx, real_address, code_address);
    if (block != NULL)
    {
      if (block->recycle_count >= ctx->stalker->priv->trust_threshold ||
          memcmp (real_address, block->real_snapshot,
            block->real_end - block->real_begin) == 0)
      {
        block->recycle_count++;
        return block;
      }
      else
      {
        gum_metal_hash_table_remove (ctx->mappings, real_address);
      }
    }
  }

  block = gum_exec_block_new (ctx);
  *code_address = block->code_begin;

  if (ctx->stalker->priv->trust_threshold >= 0)
    gum_metal_hash_table_insert (ctx->mappings, real_address, block);

  cw = &ctx->code_writer;
  rl = &ctx->relocator;

  gum_x86_writer_reset (cw, block->code_begin);
  gum_x86_relocator_reset (rl, real_address, cw);

  gc.instruction = NULL;
  gc.relocator = rl;
  gc.code_writer = cw;
  gc.continuation_real_address = NULL;
  gc.opened_prolog = GUM_PROLOG_NONE;
  gc.accumulated_stack_delta = 0;

#if ENABLE_DEBUG
  printf ("\n\n***\n\nCreating block for %p:\n", real_address);
#endif

  iterator.exec_context = ctx;
  iterator.exec_block = block;
  iterator.generator_context = &gc;

  iterator.instruction.ci = NULL;
  iterator.instruction.begin = NULL;
  iterator.instruction.end = NULL;
  iterator.requirements = GUM_REQUIRE_NOTHING;

  gum_stalker_transformer_transform_block (ctx->transformer, &iterator,
      (GumStalkerWriter *) cw);

  if (gc.continuation_real_address != NULL)
  {
    GumBranchTarget continue_target = { 0, };

    continue_target.is_indirect = FALSE;
    continue_target.absolute_address = gc.continuation_real_address;

    gum_exec_block_write_jmp_transfer_code (block, &continue_target,
        GUM_ENTRYGATE (jmp_continuation), &gc);
  }

  gum_x86_writer_put_breakpoint (cw); /* Should never get here */

  all_labels_resolved = gum_x86_writer_flush (cw);
  g_assert (all_labels_resolved);

  block->code_end = (guint8 *) gum_x86_writer_cur (cw);

  block->real_begin = (guint8 *) rl->input_start;
  block->real_end = (guint8 *) rl->input_cur;

  gum_exec_block_commit (block);

  if ((ctx->sink_mask & GUM_COMPILE) != 0)
  {
    ctx->tmp_event.type = GUM_COMPILE;
    ctx->tmp_event.compile.begin = block->real_begin;
    ctx->tmp_event.compile.end = block->real_end;

    gum_event_sink_process (ctx->sink, &ctx->tmp_event);
  }

  return block;
}

gboolean
gum_stalker_iterator_next (GumStalkerIterator * self,
                           const cs_insn ** insn)
{
  GumGeneratorContext * gc = self->generator_context;
  GumX86Relocator * rl = gc->relocator;
  GumInstruction * instruction;
  guint n_read;

  instruction = self->generator_context->instruction;
  if (instruction != NULL)
  {
    GumExecBlock * block = self->exec_block;
    gboolean skip_implicitly_requested;

    skip_implicitly_requested = rl->outpos != rl->inpos;
    if (skip_implicitly_requested)
    {
      gum_x86_relocator_skip_one_no_label (rl);
    }

#if ENABLE_DEBUG
    {
      guint8 * begin = block->code_end;
      block->code_end = gum_x86_writer_cur (gc->code_writer);
      gum_disasm (begin, block->code_end - begin, "\t");
      gum_hexdump (begin, block->code_end - begin, "\t; ");
    }
#else
    block->code_end = gum_x86_writer_cur (gc->code_writer);
#endif

    if (gum_exec_block_is_full (block))
    {
      gc->continuation_real_address = instruction->end;
      return FALSE;
    }
    else if (instruction->ci->id == X86_INS_CALL)
    {
      gboolean call_is_to_excluded_range;

      call_is_to_excluded_range =
          (self->requirements & GUM_REQUIRE_RELOCATION) != 0;
      if (!call_is_to_excluded_range)
        return FALSE;
    }
    else if (gum_x86_relocator_eob (rl))
    {
      return FALSE;
    }
  }

  instruction = &self->instruction;

  n_read = gum_x86_relocator_read_one (rl, &instruction->ci);
  if (n_read == 0)
    return FALSE;

  instruction->begin = GSIZE_TO_POINTER (instruction->ci->address);
  instruction->end = instruction->begin + instruction->ci->size;

#if ENABLE_DEBUG
  gum_disasm (instruction->begin, instruction->end - instruction->begin, "");
  gum_hexdump (instruction->begin, instruction->end - instruction->begin, "; ");
#endif

  self->generator_context->instruction = instruction;

  if (insn != NULL)
    *insn = instruction->ci;

  return TRUE;
}

void
gum_stalker_iterator_keep (GumStalkerIterator * self)
{
  GumExecCtx * ec = self->exec_context;
  GumExecBlock * block = self->exec_block;
  GumGeneratorContext * gc = self->generator_context;
  GumX86Relocator * rl = gc->relocator;
  const cs_insn * insn = gc->instruction->ci;
  GumVirtualizationRequirements requirements;

  if ((ec->sink_mask & GUM_EXEC) != 0)
    gum_exec_block_write_exec_event_code (block, gc, GUM_CODE_INTERRUPTIBLE);

  if ((ec->sink_mask & GUM_BLOCK) != 0 &&
      gum_x86_relocator_eob (rl) &&
      insn->id != X86_INS_CALL)
  {
    gum_exec_block_write_block_event_code (block, gc, GUM_CODE_INTERRUPTIBLE);
  }

  switch (insn->id)
  {
    case X86_INS_CALL:
    case X86_INS_JMP:
      requirements = gum_exec_block_virtualize_branch_insn (block, gc);
      break;
    case X86_INS_RET:
      requirements = gum_exec_block_virtualize_ret_insn (block, gc);
      break;
    case X86_INS_SYSENTER:
      requirements = gum_exec_block_virtualize_sysenter_insn (block, gc);
      break;
    case X86_INS_JECXZ:
    case X86_INS_JRCXZ:
      requirements = gum_exec_block_virtualize_branch_insn (block, gc);
      break;
    default:
      if (gum_x86_reader_insn_is_jcc (insn))
        requirements = gum_exec_block_virtualize_branch_insn (block, gc);
      else
        requirements = GUM_REQUIRE_RELOCATION;
      break;
  }

  gum_exec_block_close_prolog (block, gc);

  if ((requirements & GUM_REQUIRE_RELOCATION) != 0)
  {
    gum_x86_relocator_write_one_no_label (rl);
  }
  else if ((requirements & GUM_REQUIRE_SINGLE_STEP) != 0)
  {
    gum_x86_relocator_skip_one_no_label (rl);
    gum_exec_block_write_single_step_transfer_code (block, gc);
  }

  self->requirements = requirements;
}

static void
gum_exec_ctx_emit_call_event (GumExecCtx * ctx,
                              gpointer location,
                              gpointer target)
{
  GumEvent ev;
  GumCallEvent * call = &ev.call;

  ev.type = GUM_CALL;

  call->location = location;
  call->target = target;
  call->depth = ctx->first_frame - ctx->current_frame;

  ctx->sink_process_impl (ctx->sink, &ev);
}

static void
gum_exec_ctx_emit_ret_event (GumExecCtx * ctx,
                             gpointer location)
{
  GumEvent ev;
  GumRetEvent * ret = &ev.ret;

  ev.type = GUM_RET;

  ret->location = location;
  ret->target = *((gpointer *) ctx->app_stack);
  ret->depth = ctx->first_frame - ctx->current_frame;

  ctx->sink_process_impl (ctx->sink, &ev);
}

static void
gum_exec_ctx_emit_exec_event (GumExecCtx * ctx,
                              gpointer location)
{
  GumEvent ev;
  GumExecEvent * exec = &ev.exec;

  ev.type = GUM_EXEC;

  exec->location = location;

  ctx->sink_process_impl (ctx->sink, &ev);
}

static void
gum_exec_ctx_emit_block_event (GumExecCtx * ctx,
                               gpointer begin,
                               gpointer end)
{
  GumEvent ev;
  GumBlockEvent * block = &ev.block;

  ev.type = GUM_BLOCK;

  block->begin = begin;
  block->end = end;

  ctx->sink_process_impl (ctx->sink, &ev);
}

void
gum_stalker_iterator_put_callout (GumStalkerIterator * self,
                                  GumStalkerCallout callout,
                                  gpointer data,
                                  GDestroyNotify data_destroy)
{
  GumCalloutEntry * entry;
  GumExecCtx * ec = self->exec_context;
  GumExecBlock * block = self->exec_block;
  GumGeneratorContext * gc = self->generator_context;
  GumX86Writer * cw = gc->code_writer;

  entry = g_slice_new (GumCalloutEntry);
  entry->callout = callout;
  entry->data = data;
  entry->data_destroy = data_destroy;
  entry->pc = gc->instruction->begin;
  entry->exec_context = ec;

  gum_exec_block_open_prolog (block, GUM_PROLOG_FULL, gc);

  gum_x86_writer_put_call_address_with_aligned_arguments (cw,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_stalker_invoke_callout), 2,
      GUM_ARG_REGISTER, GUM_REG_XBX,
      GUM_ARG_ADDRESS, GUM_ADDRESS (entry));

  gum_exec_block_close_prolog (block, gc);

  gum_spinlock_acquire (&ec->callout_lock);
  g_queue_push_head (&ec->callout_entries, entry);
  gum_spinlock_release (&ec->callout_lock);
}

static void
gum_stalker_invoke_callout (GumCpuContext * cpu_context,
                            GumCalloutEntry * entry)
{
  GumExecCtx * ec = entry->exec_context;

  GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (entry->pc);

  gum_spinlock_acquire (&ec->callout_lock);

  if (entry->callout != NULL)
  {
    entry->callout (cpu_context, entry->data);
  }

  gum_spinlock_release (&ec->callout_lock);
}

static void
gum_exec_ctx_write_prolog (GumExecCtx * ctx,
                           GumPrologType type,
                           GumX86Writer * cw)
{
  switch (type)
  {
    case GUM_PROLOG_MINIMAL:
    case GUM_PROLOG_FULL:
    {
      gpointer helper;

      helper = (type == GUM_PROLOG_MINIMAL)
          ? ctx->last_prolog_minimal
          : ctx->last_prolog_full;

      gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
          GUM_REG_XSP, -GUM_RED_ZONE_SIZE);
      gum_x86_writer_put_call_address (cw, GUM_ADDRESS (helper));

      break;
    }
    case GUM_PROLOG_IC:
    {
      gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
          GUM_REG_XSP, -GUM_RED_ZONE_SIZE);
      gum_x86_writer_put_pushfx (cw);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XBX);
      gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XBX, GUM_REG_XSP);

      gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XAX, GUM_REG_XSP,
          3 * sizeof (gpointer) + GUM_RED_ZONE_SIZE);
      gum_x86_writer_put_mov_near_ptr_reg (cw, GUM_ADDRESS (&ctx->app_stack),
          GUM_REG_XAX);

      break;
    }
    default:
    {
      g_assert_not_reached ();
      break;
    }
  }
}

static void
gum_exec_ctx_write_epilog (GumExecCtx * ctx,
                           GumPrologType type,
                           GumX86Writer * cw)
{
  switch (type)
  {
    case GUM_PROLOG_MINIMAL:
    case GUM_PROLOG_FULL:
    {
      gpointer helper;

      helper = (type == GUM_PROLOG_MINIMAL)
          ? ctx->last_epilog_minimal
          : ctx->last_epilog_full;

      gum_x86_writer_put_call_address (cw, GUM_ADDRESS (helper));
      gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XSP,
          GUM_ADDRESS (&ctx->app_stack));

      break;
    }
    case GUM_PROLOG_IC:
    {
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XBX);
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_popfx (cw);
      gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XSP,
          GUM_ADDRESS (&ctx->app_stack));

      break;
    }
    default:
    {
      g_assert_not_reached ();
      break;
    }
  }
}

static void
gum_exec_ctx_ensure_inline_helpers_reachable (GumExecCtx * ctx)
{
  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_prolog_minimal,
      gum_exec_ctx_write_minimal_prolog_helper);
  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_epilog_minimal,
      gum_exec_ctx_write_minimal_epilog_helper);

  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_prolog_full,
      gum_exec_ctx_write_full_prolog_helper);
  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_epilog_full,
      gum_exec_ctx_write_full_epilog_helper);

  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_stack_push,
      gum_exec_ctx_write_stack_push_helper);
  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_stack_pop_and_go,
      gum_exec_ctx_write_stack_pop_and_go_helper);
}

static void
gum_exec_ctx_write_minimal_prolog_helper (GumExecCtx * ctx,
                                          GumX86Writer * cw)
{
  gum_exec_ctx_write_prolog_helper (ctx, GUM_PROLOG_MINIMAL, cw);
}

static void
gum_exec_ctx_write_minimal_epilog_helper (GumExecCtx * ctx,
                                          GumX86Writer * cw)
{
  gum_exec_ctx_write_epilog_helper (ctx, GUM_PROLOG_MINIMAL, cw);
}

static void
gum_exec_ctx_write_full_prolog_helper (GumExecCtx * ctx,
                                       GumX86Writer * cw)
{
  gum_exec_ctx_write_prolog_helper (ctx, GUM_PROLOG_FULL, cw);
}

static void
gum_exec_ctx_write_full_epilog_helper (GumExecCtx * ctx,
                                       GumX86Writer * cw)
{
  gum_exec_ctx_write_epilog_helper (ctx, GUM_PROLOG_FULL, cw);
}

static void
gum_exec_ctx_write_prolog_helper (GumExecCtx * ctx,
                                  GumPrologType type,
                                  GumX86Writer * cw)
{
  guint8 fxsave[] = {
    0x0f, 0xae, 0x04, 0x24 /* fxsave [esp] */
  };
  guint8 upper_ymm_saver[] = {
#if GLIB_SIZEOF_VOID_P == 8
    /* vextracti128 ymm0..ymm15, [rsp+0x0]..[rsp+0xF0], 1 */
    0xc4, 0xe3, 0x7d, 0x39, 0x04, 0x24, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x4c, 0x24, 0x10, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x54, 0x24, 0x20, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x5c, 0x24, 0x30, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x64, 0x24, 0x40, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x6c, 0x24, 0x50, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x74, 0x24, 0x60, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x7c, 0x24, 0x70, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0x8c, 0x24, 0x90, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0x94, 0x24, 0xa0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0x9c, 0x24, 0xb0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0xa4, 0x24, 0xc0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0xac, 0x24, 0xd0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0xb4, 0x24, 0xe0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0xbc, 0x24, 0xf0, 0x00, 0x00, 0x00, 0x01
#else
    /* vextracti128 ymm0..ymm7, [esp+0x0]..[esp+0x70], 1 */
    0xc4, 0xc3, 0x7d, 0x39, 0x04, 0x24, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x4c, 0x24, 0x10, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x54, 0x24, 0x20, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x5c, 0x24, 0x30, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x64, 0x24, 0x40, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x6c, 0x24, 0x50, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x74, 0x24, 0x60, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x7c, 0x24, 0x70, 0x01
#endif
  };

  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_cld (cw); /* C ABI mandates this */

  if (type == GUM_PROLOG_MINIMAL)
  {
    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);

    gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XAX, GUM_REG_XSP,
        3 * sizeof (gpointer) + GUM_RED_ZONE_SIZE);
    gum_x86_writer_put_mov_near_ptr_reg (cw, GUM_ADDRESS (&ctx->app_stack),
        GUM_REG_XAX);

    gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XBX);

#if GLIB_SIZEOF_VOID_P == 8
    gum_x86_writer_put_push_reg (cw, GUM_REG_XSI);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDI);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R8);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R9);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R10);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R11);
#endif
  }
  else /* GUM_PROLOG_FULL */
  {
    gum_x86_writer_put_pushax (cw); /* All of GumCpuContext except for xip */
    /* GumCpuContext.xip gets filled out later */
    gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP, GUM_REG_XSP,
        -((gint) sizeof (gpointer)));

    gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XAX, GUM_REG_XSP,
        sizeof (GumCpuContext) + 2 * sizeof (gpointer) + GUM_RED_ZONE_SIZE);
    gum_x86_writer_put_mov_near_ptr_reg (cw, GUM_ADDRESS (&ctx->app_stack),
        GUM_REG_XAX);

    gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
        GUM_REG_XSP, GUM_CPU_CONTEXT_OFFSET_XSP,
        GUM_REG_XAX);
  }

  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XBX, GUM_REG_XSP);
  gum_x86_writer_put_and_reg_u32 (cw, GUM_REG_XSP, (guint32) ~(16 - 1));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP, 512);
  gum_x86_writer_put_bytes (cw, fxsave, sizeof (fxsave));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP, 0x100);
  gum_x86_writer_put_bytes (cw, upper_ymm_saver, sizeof (upper_ymm_saver));

  /* Jump to our caller but leave it on the stack */
  gum_x86_writer_put_jmp_reg_offset_ptr (cw,
      GUM_REG_XBX, (type == GUM_PROLOG_MINIMAL)
          ? GUM_MINIMAL_PROLOG_RETURN_OFFSET
          : GUM_FULL_PROLOG_RETURN_OFFSET);
}

static void
gum_exec_ctx_write_epilog_helper (GumExecCtx * ctx,
                                  GumPrologType type,
                                  GumX86Writer * cw)
{
  guint8 fxrstor[] = {
    0x0f, 0xae, 0x0c, 0x24 /* fxrstor [esp] */
  };
  guint8 upper_ymm_restorer[] = {
#if GLIB_SIZEOF_VOID_P == 8
    /* vinserti128 ymm0..ymm15, ymm0..ymm15, [rsp+0x0]..[rsp+0xF0], 1 */
    0xc4, 0xe3, 0x7d, 0x38, 0x04, 0x24, 0x01,
    0xc4, 0xe3, 0x75, 0x38, 0x4c, 0x24, 0x10, 0x01,
    0xc4, 0xe3, 0x6d, 0x38, 0x54, 0x24, 0x20, 0x01,
    0xc4, 0xe3, 0x65, 0x38, 0x5c, 0x24, 0x30, 0x01,
    0xc4, 0xe3, 0x5d, 0x38, 0x64, 0x24, 0x40, 0x01,
    0xc4, 0xe3, 0x55, 0x38, 0x6c, 0x24, 0x50, 0x01,
    0xc4, 0xe3, 0x4d, 0x38, 0x74, 0x24, 0x60, 0x01,
    0xc4, 0xe3, 0x45, 0x38, 0x7c, 0x24, 0x70, 0x01,
    0xc4, 0x63, 0x3d, 0x38, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x35, 0x38, 0x8c, 0x24, 0x90, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x2d, 0x38, 0x94, 0x24, 0xa0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x25, 0x38, 0x9c, 0x24, 0xb0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x1d, 0x38, 0xa4, 0x24, 0xc0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x15, 0x38, 0xac, 0x24, 0xd0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x0d, 0x38, 0xb4, 0x24, 0xe0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x05, 0x38, 0xbc, 0x24, 0xf0, 0x00, 0x00, 0x00, 0x01
#else
    /* vinserti128 ymm0..ymm7, ymm0..ymm7, [esp+0x0]..[esp+0x70], 1 */
    0xc4, 0xc3, 0x7d, 0x38, 0x04, 0x24, 0x01,
    0xc4, 0xc3, 0x75, 0x38, 0x4c, 0x24, 0x10, 0x01,
    0xc4, 0xc3, 0x6d, 0x38, 0x54, 0x24, 0x20, 0x01,
    0xc4, 0xc3, 0x65, 0x38, 0x5c, 0x24, 0x30, 0x01,
    0xc4, 0xc3, 0x5d, 0x38, 0x64, 0x24, 0x40, 0x01,
    0xc4, 0xc3, 0x55, 0x38, 0x6c, 0x24, 0x50, 0x01,
    0xc4, 0xc3, 0x4d, 0x38, 0x74, 0x24, 0x60, 0x01,
    0xc4, 0xc3, 0x45, 0x38, 0x7c, 0x24, 0x70, 0x01
#endif
  };

  /* Store our caller in the return address created by the prolog */
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XBX, (type == GUM_PROLOG_MINIMAL)
          ? GUM_MINIMAL_PROLOG_RETURN_OFFSET
          : GUM_FULL_PROLOG_RETURN_OFFSET,
      GUM_REG_XAX);

  gum_x86_writer_put_bytes (cw, upper_ymm_restorer,
      sizeof (upper_ymm_restorer));
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP, 0x100);
  gum_x86_writer_put_bytes (cw, fxrstor, sizeof (fxrstor));
  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XSP, GUM_REG_XBX);

  if (type == GUM_PROLOG_MINIMAL)
  {
#if GLIB_SIZEOF_VOID_P == 8
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R11);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R10);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R9);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R8);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDI);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XSI);
#endif

    gum_x86_writer_put_pop_reg (cw, GUM_REG_XBX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  }
  else /* GUM_PROLOG_FULL */
  {
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX); /* Discard
                                                     GumCpuContext.xip */
    gum_x86_writer_put_popax (cw);
  }

  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_ret (cw);
}

static void
gum_exec_ctx_write_stack_push_helper (GumExecCtx * ctx,
                                      GumX86Writer * cw)
{
  gconstpointer skip_stack_push = cw->code + 1;

  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (&ctx->current_frame));
  gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);

  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XAX);
  gum_x86_writer_put_test_reg_u32 (cw, GUM_REG_XAX,
      ctx->stalker->priv->page_size - 1);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JE, skip_stack_push,
      GUM_UNLIKELY);

  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XAX, sizeof (GumExecFrame));

  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XAX, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumExecFrame, code_address), GUM_REG_XDX);

  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XCX, GUM_REG_XAX);
  gum_x86_writer_put_ret (cw);

  gum_x86_writer_put_label (cw, skip_stack_push);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_ret (cw);
}

static void
gum_exec_ctx_write_stack_pop_and_go_helper (GumExecCtx * ctx,
                                            GumX86Writer * cw)
{
  gconstpointer resolve_dynamically = cw->code + 1;
  GumAddress return_at = GUM_ADDRESS (&ctx->return_at);
  guint stack_delta = GUM_RED_ZONE_SIZE + sizeof (gpointer);

  /*
   * Fast path (try the stack)
   */
  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
  stack_delta += 2 * sizeof (gpointer);

  /*
   * We want to jump to the origin ret instruction after modifying the
   * return address on the stack.
   */
  gum_x86_writer_put_mov_near_ptr_reg (cw, return_at, GUM_REG_XCX);

  /* Check frame at the top of the stack */
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (&ctx->current_frame));
  gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
  stack_delta += sizeof (gpointer);
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XAX);

  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XCX, GUM_REG_XAX);
  gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw,
      GUM_REG_XSP, stack_delta,
      GUM_REG_XCX);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE,
      resolve_dynamically, GUM_UNLIKELY);

  /* Replace return address */
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_XCX,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumExecFrame, code_address));
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XSP, stack_delta,
      GUM_REG_XCX);

  /* Pop from our stack */
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XAX, sizeof (GumExecFrame));
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XCX, GUM_REG_XAX);

  /* Proceeed to block */
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
      GUM_REG_XSP, GUM_RED_ZONE_SIZE);

  gum_x86_writer_put_jmp_near_ptr (cw, return_at);

  gum_x86_writer_put_label (cw, resolve_dynamically);

  /* Clear our stack so we might resync later */
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (ctx->first_frame));
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XAX, GUM_REG_XCX);

  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
      GUM_REG_XSP, GUM_RED_ZONE_SIZE);

  /*
   * Slow path (resolve dynamically)
   */
  gum_exec_ctx_write_prolog (ctx, GUM_PROLOG_MINIMAL, cw);

  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (&ctx->app_stack));
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_THUNK_REG_ARG1, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (GUM_ENTRYGATE (ret_slow_path)));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);

  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (&ctx->app_stack));
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XCX, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XCX, GUM_REG_XAX);

  gum_exec_ctx_write_epilog (ctx, GUM_PROLOG_MINIMAL, cw);

  gum_x86_writer_put_jmp_near_ptr (cw, return_at);
}

static void
gum_exec_ctx_ensure_helper_reachable (GumExecCtx * ctx,
                                      gpointer * helper_ptr,
                                      GumExecHelperWriteFunc write)
{
  GumSlab * slab;
  GumX86Writer * cw;

  if (gum_exec_ctx_is_helper_reachable (ctx, helper_ptr))
    return;

  slab = ctx->code_slab;
  cw = &ctx->code_writer;

  gum_x86_writer_reset (cw, slab->data + slab->offset);
  *helper_ptr = gum_x86_writer_cur (cw);

  write (ctx, cw);

  gum_x86_writer_flush (cw);
  slab->offset += gum_x86_writer_offset (cw);
}

static gboolean
gum_exec_ctx_is_helper_reachable (GumExecCtx * ctx,
                                  gpointer * helper_ptr)
{
  GumAddress helper;
  GumSlab * slab;
  GumAddress start, end;

  helper = GUM_ADDRESS (*helper_ptr);
  if (helper == 0)
    return FALSE;

  slab = ctx->code_slab;

  start = GUM_ADDRESS (slab->data);
  end = start + slab->size;

  if (!gum_x86_writer_can_branch_directly_between (start, helper))
    return FALSE;

  return gum_x86_writer_can_branch_directly_between (end, helper);
}

static void
gum_exec_ctx_write_push_branch_target_address (GumExecCtx * ctx,
                                               const GumBranchTarget * target,
                                               GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;

  if (!target->is_indirect)
  {
    if (target->base == X86_REG_INVALID)
    {
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
          GUM_ADDRESS (target->absolute_address));
      gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);
    }
    else
    {
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_exec_ctx_load_real_register_into (ctx, GUM_REG_XAX,
          gum_cpu_reg_from_capstone (target->base),
          target->origin_ip,
          gc);
      gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);
    }
  }
  else if (target->base == X86_REG_INVALID && target->index == X86_REG_INVALID)
  {
    g_assert_cmpint (target->scale, ==, 1);
    g_assert (target->absolute_address != NULL);
    g_assert (target->relative_offset == 0);

    gum_write_segment_prefix (target->pfx_seg, cw);
    gum_x86_writer_put_u8 (cw, 0xff);
    gum_x86_writer_put_u8 (cw, 0x35);
    gum_x86_writer_put_bytes (cw, (guint8 *) &target->absolute_address,
        sizeof (target->absolute_address));
  }
  else
  {
    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX); /* Placeholder */

    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);

    gum_exec_ctx_load_real_register_into (ctx, GUM_REG_XAX,
        gum_cpu_reg_from_capstone (target->base),
        target->origin_ip,
        gc);
    gum_exec_ctx_load_real_register_into (ctx, GUM_REG_XDX,
        gum_cpu_reg_from_capstone (target->index),
        target->origin_ip,
        gc);
    gum_x86_writer_put_mov_reg_base_index_scale_offset_ptr (cw, GUM_REG_XAX,
        GUM_REG_XAX, GUM_REG_XDX, target->scale,
        target->relative_offset);
    gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
        GUM_REG_XSP, 2 * sizeof (gpointer),
        GUM_REG_XAX);

    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  }
}

static void
gum_exec_ctx_load_real_register_into (GumExecCtx * ctx,
                                      GumCpuReg target_register,
                                      GumCpuReg source_register,
                                      gpointer ip,
                                      GumGeneratorContext * gc)
{
  switch (gc->opened_prolog)
  {
    case GUM_PROLOG_MINIMAL:
      gum_exec_ctx_load_real_register_from_minimal_frame_into (ctx,
          target_register, source_register, ip, gc);
      break;
    case GUM_PROLOG_FULL:
      gum_exec_ctx_load_real_register_from_full_frame_into (ctx,
          target_register, source_register, ip, gc);
      break;
    case GUM_PROLOG_IC:
      gum_exec_ctx_load_real_register_from_ic_frame_into (ctx, target_register,
          source_register, ip, gc);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

static void
gum_exec_ctx_load_real_register_from_minimal_frame_into (
    GumExecCtx * ctx,
    GumCpuReg target_register,
    GumCpuReg source_register,
    gpointer ip,
    GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  GumCpuReg source_meta;

  source_meta = gum_cpu_meta_reg_from_real_reg (source_register);

  if (source_meta >= GUM_REG_XAX && source_meta <= GUM_REG_XBX)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX,
        GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX * sizeof (gpointer) -
        ((source_meta - GUM_REG_XAX) * sizeof (gpointer)));
  }
#if GLIB_SIZEOF_VOID_P == 8
  else if (source_meta >= GUM_REG_XSI && source_meta <= GUM_REG_XDI)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX,
        GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX * sizeof (gpointer) -
        ((source_meta - 2 - GUM_REG_XAX) * sizeof (gpointer)));
  }
  else if (source_meta >= GUM_REG_R8 && source_meta <= GUM_REG_R11)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX,
        GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX * sizeof (gpointer) -
        ((source_meta - 2 - GUM_REG_RAX) * sizeof (gpointer)));
  }
#endif
  else if (source_meta == GUM_REG_XSP)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, target_register,
        GUM_ADDRESS (&ctx->app_stack));
    gum_x86_writer_put_lea_reg_reg_offset (cw, target_register,
        target_register, gc->accumulated_stack_delta);
  }
  else if (source_meta == GUM_REG_XIP)
  {
    gum_x86_writer_put_mov_reg_address (cw, target_register, GUM_ADDRESS (ip));
  }
  else if (source_meta == GUM_REG_NONE)
  {
    gum_x86_writer_put_xor_reg_reg (cw, target_register, target_register);
  }
  else
  {
    gum_x86_writer_put_mov_reg_reg (cw, target_register, source_register);
  }
}

static void
gum_exec_ctx_load_real_register_from_full_frame_into (GumExecCtx * ctx,
                                                      GumCpuReg target_register,
                                                      GumCpuReg source_register,
                                                      gpointer ip,
                                                      GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  GumCpuReg source_meta;

  source_meta = gum_cpu_meta_reg_from_real_reg (source_register);

  if (source_meta >= GUM_REG_XAX && source_meta <= GUM_REG_XBX)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX, sizeof (GumCpuContext) -
        ((source_meta - GUM_REG_XAX + 1) * sizeof (gpointer)));
  }
  else if (source_meta >= GUM_REG_XBP && source_meta <= GUM_REG_XDI)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX, sizeof (GumCpuContext) -
        ((source_meta - GUM_REG_XAX + 1) * sizeof (gpointer)));
  }
#if GLIB_SIZEOF_VOID_P == 8
  else if (source_meta >= GUM_REG_R8 && source_meta <= GUM_REG_R15)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX, sizeof (GumCpuContext) -
        ((source_meta - GUM_REG_RAX + 1) * sizeof (gpointer)));
  }
#endif
  else if (source_meta == GUM_REG_XSP)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, target_register,
        GUM_ADDRESS (&ctx->app_stack));
    gum_x86_writer_put_lea_reg_reg_offset (cw, target_register,
        target_register, gc->accumulated_stack_delta);
  }
  else if (source_meta == GUM_REG_XIP)
  {
    gum_x86_writer_put_mov_reg_address (cw, target_register, GUM_ADDRESS (ip));
  }
  else if (source_meta == GUM_REG_NONE)
  {
    gum_x86_writer_put_xor_reg_reg (cw, target_register, target_register);
  }
  else
  {
    gum_x86_writer_put_mov_reg_reg (cw, target_register, source_register);
  }
}

static void
gum_exec_ctx_load_real_register_from_ic_frame_into (GumExecCtx * ctx,
                                                    GumCpuReg target_register,
                                                    GumCpuReg source_register,
                                                    gpointer ip,
                                                    GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  GumCpuReg source_meta;

  source_meta = gum_cpu_meta_reg_from_real_reg (source_register);

  if (source_meta == GUM_REG_XAX)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register, GUM_REG_XBX,
        sizeof (gpointer));
  }
  else if (source_meta == GUM_REG_XBX)
  {
    gum_x86_writer_put_mov_reg_reg_ptr (cw, target_register, GUM_REG_XBX);
  }
  else if (source_meta == GUM_REG_XSP)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, target_register,
        GUM_ADDRESS (&ctx->app_stack));
    gum_x86_writer_put_lea_reg_reg_offset (cw, target_register,
        target_register, gc->accumulated_stack_delta);
  }
  else if (source_meta == GUM_REG_XIP)
  {
    gum_x86_writer_put_mov_reg_address (cw, target_register, GUM_ADDRESS (ip));
  }
  else if (source_meta == GUM_REG_NONE)
  {
    gum_x86_writer_put_xor_reg_reg (cw, target_register, target_register);
  }
  else
  {
    gum_x86_writer_put_mov_reg_reg (cw, target_register, source_register);
  }
}

static GumExecBlock *
gum_exec_block_new (GumExecCtx * ctx)
{
  GumSlab * slab = ctx->code_slab;

  if (slab->size - slab->offset >= GUM_EXEC_BLOCK_MIN_SIZE)
  {
    GumExecBlock * block = (GumExecBlock *) (slab->data + slab->offset);

    block->ctx = ctx;
    block->slab = slab;

    block->code_begin = GUM_ALIGN_POINTER (guint8 *,
        slab->data + slab->offset + sizeof (GumExecBlock),
        GUM_CODE_ALIGNMENT);
    block->code_end = block->code_begin;

    block->state = GUM_EXEC_NORMAL;
    block->recycle_count = 0;
    block->has_call_to_excluded_range = FALSE;

    slab->offset += block->code_begin - (slab->data + slab->offset);

    return block;
  }

  if (ctx->stalker->priv->trust_threshold < 0)
  {
    ctx->code_slab->offset = 0;

    return gum_exec_block_new (ctx);
  }

  slab = gum_alloc_n_pages (GUM_CODE_SLAB_SIZE_IN_PAGES, GUM_PAGE_RWX);
  slab->data = (guint8 *) (slab + 1);
  slab->offset = 0;
  slab->size = (GUM_CODE_SLAB_SIZE_IN_PAGES * ctx->stalker->priv->page_size)
      - sizeof (GumSlab);
  slab->next = ctx->code_slab;
  ctx->code_slab = slab;

  gum_exec_ctx_ensure_inline_helpers_reachable (ctx);

  return gum_exec_block_new (ctx);
}

static GumExecBlock *
gum_exec_block_obtain (GumExecCtx * ctx,
                       gpointer real_address,
                       gpointer * code_address)
{
  GumExecBlock * block;

  block = gum_metal_hash_table_lookup (ctx->mappings, real_address);
  if (block != NULL)
    *code_address = block->code_begin;

  return block;
}

static gboolean
gum_exec_block_is_full (GumExecBlock * block)
{
  guint8 * slab_end = block->slab->data + block->slab->size;
  return slab_end - block->code_end < GUM_EXEC_BLOCK_MIN_SIZE;
}

static void
gum_exec_block_commit (GumExecBlock * block)
{
  guint real_size;
  guint8 * aligned_end;

  real_size = block->real_end - block->real_begin;
  block->real_snapshot = block->code_end;
  memcpy (block->real_snapshot, block->real_begin, real_size);
  block->slab->offset += real_size;

  aligned_end = GUM_ALIGN_POINTER (guint8 *, block->real_snapshot + real_size,
      GUM_DATA_ALIGNMENT);
  block->slab->offset += aligned_end - block->code_begin;
}

static void
gum_exec_block_backpatch_call (GumExecBlock * block,
                               gpointer code_start,
                               GumPrologType opened_prolog,
                               gpointer ret_real_address,
                               gpointer ret_code_address)
{
  gboolean just_unfollowed;
  GumExecCtx * ctx;

  just_unfollowed = block == NULL;
  if (just_unfollowed)
    return;

  ctx = block->ctx;

  if (ctx->state == GUM_EXEC_CTX_ACTIVE &&
      block->recycle_count >= ctx->stalker->priv->trust_threshold)
  {
    GumX86Writer * cw = &ctx->code_writer;

    gum_x86_writer_reset (cw, code_start);

    if (opened_prolog == GUM_PROLOG_NONE)
    {
      gum_x86_writer_put_pushfx (cw);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);
    }

    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
        GUM_ADDRESS (ret_real_address));
    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XDX,
        GUM_ADDRESS (ret_code_address));
    gum_x86_writer_put_call_address (cw,
        GUM_ADDRESS (block->ctx->last_stack_push));

    if (opened_prolog == GUM_PROLOG_NONE)
    {
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_popfx (cw);
    }
    else
    {
      gum_exec_ctx_write_epilog (block->ctx, opened_prolog, cw);
    }

    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
        GUM_ADDRESS (ret_real_address));
    gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);

    gum_x86_writer_put_jmp_address (cw, GUM_ADDRESS (block->code_begin));

    gum_x86_writer_flush (cw);
  }
}

static void
gum_exec_block_backpatch_jmp (GumExecBlock * block,
                              gpointer code_start,
                              GumPrologType opened_prolog)
{
  gboolean just_unfollowed;
  GumExecCtx * ctx;

  just_unfollowed = block == NULL;
  if (just_unfollowed)
    return;

  ctx = block->ctx;

  if (ctx->state == GUM_EXEC_CTX_ACTIVE &&
      block->recycle_count >= ctx->stalker->priv->trust_threshold)
  {
    GumX86Writer * cw = &ctx->code_writer;

    gum_x86_writer_reset (cw, code_start);

    if (opened_prolog != GUM_PROLOG_NONE)
    {
      gum_exec_ctx_write_epilog (block->ctx, opened_prolog, cw);
    }

    gum_x86_writer_put_jmp_address (cw, GUM_ADDRESS (block->code_begin));
    gum_x86_writer_flush (cw);
  }
}

static void
gum_exec_block_backpatch_ret (GumExecBlock * block,
                              gpointer code_start)
{
  gboolean just_unfollowed;
  GumExecCtx * ctx;

  just_unfollowed = block == NULL;
  if (just_unfollowed)
    return;

  ctx = block->ctx;

  if (ctx->state == GUM_EXEC_CTX_ACTIVE &&
      block->recycle_count >= ctx->stalker->priv->trust_threshold)
  {
    GumX86Writer * cw = &ctx->code_writer;

    gum_x86_writer_reset (cw, code_start);
    gum_x86_writer_put_jmp_address (cw, GUM_ADDRESS (block->code_begin));
    gum_x86_writer_flush (cw);
  }
}

static void
gum_exec_block_backpatch_inline_cache (GumExecBlock * block,
                                       gpointer * ic_entries)
{
  gboolean just_unfollowed;
  GumExecCtx * ctx;

  just_unfollowed = block == NULL;
  if (just_unfollowed)
    return;

  ctx = block->ctx;

  if (ctx->state == GUM_EXEC_CTX_ACTIVE &&
      block->recycle_count >= ctx->stalker->priv->trust_threshold)
  {
    guint offset;

    offset = (ic_entries[0] == NULL) ? 0 : 2;

    if (ic_entries[offset + 0] == NULL)
    {
      ic_entries[offset + 0] = block->real_begin;
      ic_entries[offset + 1] = block->code_begin;
    }
  }
}

static GumVirtualizationRequirements
gum_exec_block_virtualize_branch_insn (GumExecBlock * block,
                                       GumGeneratorContext * gc)
{
  GumInstruction * insn = gc->instruction;
  GumX86Writer * cw = gc->code_writer;
  gboolean is_conditional;
  cs_x86 * x86 = &insn->ci->detail->x86;
  cs_x86_op * op = &x86->operands[0];
  GumBranchTarget target = { 0, };

  is_conditional =
      (insn->ci->id != X86_INS_CALL && insn->ci->id != X86_INS_JMP);

  target.origin_ip = insn->end;

  if (op->type == X86_OP_IMM)
  {
    target.absolute_address = GSIZE_TO_POINTER (op->imm);
    target.is_indirect = FALSE;
    target.pfx_seg = X86_REG_INVALID;
    target.base = X86_REG_INVALID;
    target.index = X86_REG_INVALID;
    target.scale = 0;
  }
  else if (op->type == X86_OP_MEM)
  {
#ifdef G_OS_WIN32
    /* Can't follow WoW64 */
    if (op->mem.segment == X86_REG_FS && op->mem.disp == 0xc0)
      return GUM_REQUIRE_SINGLE_STEP;
#endif

    if (op->mem.base == X86_REG_INVALID && op->mem.index == X86_REG_INVALID)
      target.absolute_address = GSIZE_TO_POINTER (op->mem.disp);
    else
      target.relative_offset = op->mem.disp;

    target.is_indirect = TRUE;
    target.pfx_seg = op->mem.segment;
    target.base = op->mem.base;
    target.index = op->mem.index;
    target.scale = op->mem.scale;
  }
  else if (op->type == X86_OP_REG)
  {
    target.is_indirect = FALSE;
    target.pfx_seg = X86_REG_INVALID;
    target.base = op->reg;
    target.index = X86_REG_INVALID;
    target.scale = 0;
  }
  else
  {
    g_assert_not_reached ();
  }

  if (insn->ci->id == X86_INS_CALL)
  {
    gboolean target_is_excluded = FALSE;

    if ((block->ctx->sink_mask & GUM_CALL) != 0)
    {
      gum_exec_block_write_call_event_code (block, &target, gc,
          GUM_CODE_INTERRUPTIBLE);
    }

    if (block->ctx->stalker->priv->any_probes_attached)
      gum_exec_block_write_call_probe_code (block, &target, gc);

    if (!target.is_indirect && target.base == X86_REG_INVALID)
    {
      GArray * exclusions = block->ctx->stalker->priv->exclusions;
      guint i;

      for (i = 0; i != exclusions->len; i++)
      {
        GumMemoryRange * r = &g_array_index (exclusions, GumMemoryRange, i);
        if (GUM_MEMORY_RANGE_INCLUDES (r,
            GUM_ADDRESS (target.absolute_address)))
        {
          target_is_excluded = TRUE;
          break;
        }
      }
    }

    if (target_is_excluded)
    {
      block->has_call_to_excluded_range = TRUE;
      return GUM_REQUIRE_RELOCATION;
    }

    gum_x86_relocator_skip_one_no_label (gc->relocator);
    gum_exec_block_write_call_invoke_code (block, &target, gc);
  }
  else if (insn->ci->id == X86_INS_JECXZ || insn->ci->id == X86_INS_JRCXZ)
  {
    gpointer is_true, is_false;
    GumBranchTarget false_target = { 0, };

    gum_x86_relocator_skip_one_no_label (gc->relocator);

    is_true =
        GUINT_TO_POINTER ((GPOINTER_TO_UINT (insn->begin) << 16) | 0xbeef);
    is_false =
        GUINT_TO_POINTER ((GPOINTER_TO_UINT (insn->begin) << 16) | 0xbabe);

    gum_exec_block_close_prolog (block, gc);

    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JCXZ, is_true, GUM_NO_HINT);
    gum_x86_writer_put_jmp_near_label (cw, is_false);

    gum_x86_writer_put_label (cw, is_true);
    gum_exec_block_write_jmp_transfer_code (block, &target,
        GUM_ENTRYGATE (jmp_cond_jcxz), gc);

    gum_x86_writer_put_label (cw, is_false);
    false_target.is_indirect = FALSE;
    false_target.absolute_address = insn->end;
    gum_exec_block_write_jmp_transfer_code (block, &false_target,
        GUM_ENTRYGATE (jmp_cond_jcxz), gc);
  }
  else
  {
    gpointer is_false;
    GumExecCtxReplaceCurrentBlockFunc regular_entry_func, cond_entry_func;

    gum_x86_relocator_skip_one_no_label (gc->relocator);

    is_false =
        GUINT_TO_POINTER ((GPOINTER_TO_UINT (insn->begin) << 16) | 0xbeef);

    if (is_conditional)
    {
      g_assert (!target.is_indirect);

      gum_exec_block_close_prolog (block, gc);

      gum_x86_writer_put_jcc_near_label (cw, gum_negate_jcc (insn->ci->id),
          is_false, GUM_NO_HINT);
    }

    if (target.is_indirect)
    {
      regular_entry_func = GUM_ENTRYGATE (jmp_mem);
      cond_entry_func = GUM_ENTRYGATE (jmp_cond_mem);
    }
    else if (target.base != X86_REG_INVALID)
    {
      regular_entry_func = GUM_ENTRYGATE (jmp_reg);
      cond_entry_func = GUM_ENTRYGATE (jmp_cond_reg);
    }
    else
    {
      regular_entry_func = GUM_ENTRYGATE (jmp_imm);
      cond_entry_func = GUM_ENTRYGATE (jmp_cond_imm);
    }

    gum_exec_block_write_jmp_transfer_code (block, &target,
        is_conditional ? cond_entry_func : regular_entry_func, gc);

    if (is_conditional)
    {
      GumBranchTarget cond_target = { 0, };

      cond_target.is_indirect = FALSE;
      cond_target.absolute_address = insn->end;

      gum_x86_writer_put_label (cw, is_false);
      gum_exec_block_write_jmp_transfer_code (block, &cond_target,
          cond_entry_func, gc);
    }
  }

  return GUM_REQUIRE_NOTHING;
}

static GumVirtualizationRequirements
gum_exec_block_virtualize_ret_insn (GumExecBlock * block,
                                    GumGeneratorContext * gc)
{
  if ((block->ctx->sink_mask & GUM_RET) != 0)
    gum_exec_block_write_ret_event_code (block, gc, GUM_CODE_INTERRUPTIBLE);

  gum_x86_relocator_skip_one_no_label (gc->relocator);

  gum_exec_block_write_ret_transfer_code (block, gc);

  return GUM_REQUIRE_NOTHING;
}

static GumVirtualizationRequirements
gum_exec_block_virtualize_sysenter_insn (GumExecBlock * block,
                                         GumGeneratorContext * gc)
{
#if GLIB_SIZEOF_VOID_P == 4 && !defined (HAVE_QNX)
  GumX86Writer * cw = gc->code_writer;
#if defined (HAVE_WINDOWS)
  guint8 code[] = {
    /* 00 */ 0x50,                                /* push eax              */
    /* 01 */ 0x8b, 0x02,                          /* mov eax, [edx]        */
    /* 03 */ 0xa3, 0xaa, 0xaa, 0xaa, 0xaa,        /* mov [0xaaaaaaaa], eax */
    /* 08 */ 0xc7, 0x02, 0xbb, 0xbb, 0xbb, 0xbb,  /* mov [edx], 0xbbbbbbbb */
    /* 0e */ 0x58,                                /* pop eax               */
    /* 0f */ 0x0f, 0x34,                          /* sysenter              */
    /* 11 */ 0xcc, 0xcc, 0xcc, 0xcc               /* <saved ret-addr here> */
  };
  const gsize store_ret_addr_offset = 0x03 + 1;
  const gsize load_continuation_addr_offset = 0x08 + 2;
  const gsize saved_ret_addr_offset = 0x11;
#elif defined (HAVE_DARWIN)
  guint8 code[] = {
    /* 00 */ 0x89, 0x15, 0xaa, 0xaa, 0xaa, 0xaa, /* mov [0xaaaaaaaa], edx */
    /* 06 */ 0xba, 0xbb, 0xbb, 0xbb, 0xbb,       /* mov edx, 0xbbbbbbbb   */
    /* 0b */ 0x0f, 0x34,                         /* sysenter              */
    /* 0d */ 0xcc, 0xcc, 0xcc, 0xcc              /* <saved ret-addr here> */
  };
  const gsize store_ret_addr_offset = 0x00 + 2;
  const gsize load_continuation_addr_offset = 0x06 + 1;
  const gsize saved_ret_addr_offset = 0x0d;
#elif defined (HAVE_LINUX)
  guint8 code[] = {
    /* 00 */ 0x8b, 0x54, 0x24, 0x0c,             /* mov edx, [esp + 12]   */
    /* 04 */ 0x89, 0x15, 0xaa, 0xaa, 0xaa, 0xaa, /* mov [0xaaaaaaaa], edx */
    /* 0a */ 0xba, 0xbb, 0xbb, 0xbb, 0xbb,       /* mov edx, 0xbbbbbbbb   */
    /* 0f */ 0x89, 0x54, 0x24, 0x0c,             /* mov [esp + 12], edx   */
    /* 13 */ 0x8b, 0x54, 0x24, 0x04,             /* mov edx, [esp + 4]    */
    /* 17 */ 0x0f, 0x34,                         /* sysenter              */
    /* 19 */ 0xcc, 0xcc, 0xcc, 0xcc              /* <saved ret-addr here> */
  };
  const gsize store_ret_addr_offset = 0x04 + 2;
  const gsize load_continuation_addr_offset = 0x0a + 1;
  const gsize saved_ret_addr_offset = 0x19;
#endif
  gpointer * saved_ret_addr;
  gpointer continuation;
  gconstpointer resolve_dynamically_label = cw->code;

  gum_exec_block_close_prolog (block, gc);

  saved_ret_addr = (gpointer *) (cw->code + saved_ret_addr_offset);
  continuation = cw->code + saved_ret_addr_offset + 4;
  *((gpointer *) (code + store_ret_addr_offset)) = saved_ret_addr;
  *((gpointer *) (code + load_continuation_addr_offset)) = continuation;

  gum_x86_writer_put_bytes (cw, code, sizeof (code));

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_EDX,
      GUM_ADDRESS (saved_ret_addr));

  if ((block->ctx->sink_mask & GUM_RET) != 0)
  {
    gum_exec_block_write_ret_event_code (block, gc, GUM_CODE_UNINTERRUPTIBLE);
    gum_exec_block_close_prolog (block, gc);
  }

  /*
   * Fast path (try the stack)
   */
  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_push_reg (cw, GUM_REG_EAX);

  /* But first, check if we've been asked to unfollow,
   * in which case we'll enter the Stalker so the unfollow can
   * be completed... */
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_EAX,
      GUM_ADDRESS (&block->ctx->state));
  gum_x86_writer_put_cmp_reg_i32 (cw, GUM_REG_EAX,
      GUM_EXEC_CTX_UNFOLLOW_PENDING);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JE,
      resolve_dynamically_label, GUM_UNLIKELY);

  /* Check frame at the top of the stack */
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_EAX,
      GUM_ADDRESS (&block->ctx->current_frame));
  gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw,
      GUM_REG_EAX, G_STRUCT_OFFSET (GumExecFrame, real_address),
      GUM_REG_EDX);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE,
      resolve_dynamically_label, GUM_UNLIKELY);

  /* Replace return address */
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_EDX,
      GUM_REG_EAX, G_STRUCT_OFFSET (GumExecFrame, code_address));

  /* Pop from our stack */
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_EAX, sizeof (GumExecFrame));
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->current_frame), GUM_REG_EAX);

  /* Proceeed to block */
  gum_x86_writer_put_pop_reg (cw, GUM_REG_EAX);
  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_jmp_reg (cw, GUM_REG_EDX);

  gum_x86_writer_put_label (cw, resolve_dynamically_label);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_EAX);
  gum_x86_writer_put_popfx (cw);

  /*
   * Slow path (resolve dynamically)
   */
  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_THUNK_REG_ARG1,
      GUM_ADDRESS (saved_ret_addr));
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_ESP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (GUM_ENTRYGATE (sysenter_slow_path)));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  gum_exec_block_close_prolog (block, gc);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));

  gum_x86_relocator_skip_one_no_label (gc->relocator);

  return GUM_REQUIRE_NOTHING;
#else
  (void) block;
  (void) gc;

  return GUM_REQUIRE_RELOCATION;
#endif
}

static void
gum_exec_block_write_call_invoke_code (GumExecBlock * block,
                                       const GumBranchTarget * target,
                                       GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  gpointer call_code_start;
  GumPrologType opened_prolog;
  gboolean can_backpatch_statically;
  gpointer * ic_entries = NULL;
  GumExecCtxReplaceCurrentBlockFunc entry_func;
  gconstpointer push_application_retaddr = cw->code + 1;
  gconstpointer perform_stack_push = cw->code + 2;
  gconstpointer look_in_cache = cw->code + 3;
  gconstpointer try_second = cw->code + 4;
  gconstpointer resolve_dynamically = cw->code + 5;
  gconstpointer beach = cw->code + 6;
  gpointer ret_real_address, ret_code_address;

  call_code_start = cw->code;
  opened_prolog = gc->opened_prolog;

  can_backpatch_statically = block->ctx->stalker->priv->trust_threshold >= 0 &&
      !target->is_indirect &&
      target->base == X86_REG_INVALID;

  if (block->ctx->stalker->priv->trust_threshold >= 0 &&
      !can_backpatch_statically)
  {
    gpointer null_ptr = NULL;
    gpointer ic1_real, ic1_code;
    gpointer ic2_real, ic2_code;

    if (opened_prolog == GUM_PROLOG_NONE)
    {
      gum_exec_block_open_prolog (block, GUM_PROLOG_IC, gc);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);
    }

    gum_x86_writer_put_call_near_label (cw, push_application_retaddr);
    gc->accumulated_stack_delta += sizeof (gpointer);

    gum_x86_writer_put_call_near_label (cw, perform_stack_push);

    if (opened_prolog == GUM_PROLOG_NONE)
    {
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    }
    else
    {
      gum_exec_block_close_prolog (block, gc);
      gum_exec_block_open_prolog (block, GUM_PROLOG_IC, gc);
      gc->accumulated_stack_delta += sizeof (gpointer);
    }

    gum_x86_writer_put_jmp_short_label (cw, look_in_cache);

    ic_entries = gum_x86_writer_cur (cw);
    ic1_real = ic_entries;
    gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));
    ic1_code = gum_x86_writer_cur (cw);
    gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));
    ic2_real = gum_x86_writer_cur (cw);
    gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));
    ic2_code = gum_x86_writer_cur (cw);
    gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));

    gum_x86_writer_put_label (cw, look_in_cache);

    gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);

    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (ic1_real));
    gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw, GUM_REG_XSP, 0, GUM_REG_XAX);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE, try_second,
        GUM_NO_HINT);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_exec_ctx_write_epilog (block->ctx, GUM_PROLOG_IC, cw);
    gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (ic1_code));

    gum_x86_writer_put_label (cw, try_second);
    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (ic2_real));
    gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw, GUM_REG_XSP, 0, GUM_REG_XAX);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE, resolve_dynamically,
        GUM_NO_HINT);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_exec_ctx_write_epilog (block->ctx, GUM_PROLOG_IC, cw);
    gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (ic2_code));

    gum_x86_writer_put_label (cw, resolve_dynamically);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_exec_block_close_prolog (block, gc);
  }

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  if (ic_entries == NULL)
  {
    gum_x86_writer_put_call_near_label (cw, push_application_retaddr);

    gum_x86_writer_put_call_near_label (cw, perform_stack_push);
  }

  gc->accumulated_stack_delta += sizeof (gpointer);

  if (target->is_indirect)
  {
    entry_func = GUM_ENTRYGATE (call_mem);
  }
  else if (target->base != X86_REG_INVALID)
  {
    entry_func = GUM_ENTRYGATE (call_reg);
  }
  else
  {
    entry_func = GUM_ENTRYGATE (call_imm);
  }

  /* Generate code for the target */
  gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
  gum_x86_writer_put_pop_reg (cw, GUM_THUNK_REG_ARG1);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (entry_func));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XDX, GUM_REG_XAX);
  gum_x86_writer_put_jmp_near_label (cw, beach);

  /* Generate code for handling the return */
  ret_real_address = gc->instruction->end;
  ret_code_address = cw->code;

  gum_exec_ctx_write_prolog (block->ctx, GUM_PROLOG_MINIMAL, cw);

  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG1,
      GUM_ADDRESS (ret_real_address));
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (GUM_ENTRYGATE (post_call_invoke)));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
      GUM_ADDRESS (&block->ctx->current_block));
  gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
      GUM_ADDRESS (gum_exec_block_backpatch_ret), 2,
      GUM_ARG_REGISTER, GUM_REG_XAX,
      GUM_ARG_ADDRESS, GUM_ADDRESS (ret_code_address));

  gum_exec_ctx_write_epilog (block->ctx, GUM_PROLOG_MINIMAL, cw);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));

  gum_x86_writer_put_label (cw, push_application_retaddr);
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
      GUM_ADDRESS (&block->ctx->app_stack));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XAX, sizeof (gpointer));
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (gc->instruction->end));
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XAX, GUM_REG_XCX);
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->app_stack), GUM_REG_XAX);
  gum_x86_writer_put_ret (cw);

  gum_x86_writer_put_label (cw, perform_stack_push);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (ret_real_address));
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XDX,
      GUM_ADDRESS (ret_code_address));
  gum_x86_writer_put_call_address (cw,
      GUM_ADDRESS (block->ctx->last_stack_push));
  gum_x86_writer_put_ret (cw);

  gum_x86_writer_put_label (cw, beach);

  if (block->ctx->stalker->priv->trust_threshold >= 0)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (&block->ctx->current_block));
  }

  if (can_backpatch_statically)
  {
    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_backpatch_call), 5,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (call_code_start),
        GUM_ARG_ADDRESS, GUM_ADDRESS (opened_prolog),
        GUM_ARG_ADDRESS, GUM_ADDRESS (ret_real_address),
        GUM_ARG_ADDRESS, GUM_ADDRESS (ret_code_address));
  }

  if (ic_entries != NULL)
  {
    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_backpatch_inline_cache), 2,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (ic_entries));
  }

  /* Execute the generated code */
  gum_exec_block_close_prolog (block, gc);

  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));
}

static void
gum_exec_block_write_jmp_transfer_code (GumExecBlock * block,
                                        const GumBranchTarget * target,
                                        GumExecCtxReplaceCurrentBlockFunc func,
                                        GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  guint8 * code_start;
  GumPrologType opened_prolog;
  gboolean can_backpatch_statically;
  gpointer * ic_entries = NULL;
  gconstpointer look_in_cache = cw->code + 1;
  gconstpointer try_second = cw->code + 2;
  gconstpointer resolve_dynamically = cw->code + 3;

  code_start = cw->code;
  opened_prolog = gc->opened_prolog;

  can_backpatch_statically = block->ctx->stalker->priv->trust_threshold >= 0 &&
      !target->is_indirect &&
      target->base == X86_REG_INVALID;

  if (block->ctx->stalker->priv->trust_threshold >= 0 &&
      !can_backpatch_statically)
  {
    gpointer null_ptr = NULL;
    gpointer ic1_real, ic1_code;
    gpointer ic2_real, ic2_code;

    gum_exec_block_close_prolog (block, gc);

    gum_x86_writer_put_jmp_short_label (cw, look_in_cache);

    ic_entries = gum_x86_writer_cur (cw);
    ic1_real = ic_entries;
    gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));
    ic1_code = gum_x86_writer_cur (cw);
    gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));
    ic2_real = gum_x86_writer_cur (cw);
    gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));
    ic2_code = gum_x86_writer_cur (cw);
    gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));

    gum_x86_writer_put_label (cw, look_in_cache);
    gum_exec_block_open_prolog (block, GUM_PROLOG_IC, gc);

    gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);

    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (ic1_real));
    gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw, GUM_REG_XSP, 0, GUM_REG_XAX);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE, try_second,
        GUM_NO_HINT);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_exec_ctx_write_epilog (block->ctx, GUM_PROLOG_IC, cw);
    gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (ic1_code));

    gum_x86_writer_put_label (cw, try_second);
    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (ic2_real));
    gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw, GUM_REG_XSP, 0, GUM_REG_XAX);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE, resolve_dynamically,
        GUM_NO_HINT);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_exec_ctx_write_epilog (block->ctx, GUM_PROLOG_IC, cw);
    gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (ic2_code));

    gum_x86_writer_put_label (cw, resolve_dynamically);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_exec_block_close_prolog (block, gc);
  }

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
  gum_x86_writer_put_pop_reg (cw, GUM_THUNK_REG_ARG1);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX, GUM_ADDRESS (func));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  if (block->ctx->stalker->priv->trust_threshold >= 0)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (&block->ctx->current_block));
  }

  if (can_backpatch_statically)
  {
    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_backpatch_jmp), 3,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (code_start),
        GUM_ARG_ADDRESS, GUM_ADDRESS (opened_prolog));
  }

  if (ic_entries != NULL)
  {
    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_backpatch_inline_cache), 2,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (ic_entries));
  }

  gum_exec_block_close_prolog (block, gc);

  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));
}

static void
gum_exec_block_write_ret_transfer_code (GumExecBlock * block,
                                        GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;

  gum_exec_block_close_prolog (block, gc);

  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
      GUM_REG_XSP, -GUM_RED_ZONE_SIZE);
  gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (gc->instruction->begin));
  gum_x86_writer_put_jmp_address (cw,
      GUM_ADDRESS (block->ctx->last_stack_pop_and_go));
}

static void
gum_exec_block_write_single_step_transfer_code (GumExecBlock * block,
                                                GumGeneratorContext * gc)
{
  guint8 code[] = {
    0xc6, 0x05, 0x78, 0x56, 0x34, 0x12,       /* mov byte [X], state */
          GUM_EXEC_SINGLE_STEPPING_ON_CALL,
    0x9c,                                     /* pushfd              */
    0x81, 0x0c, 0x24, 0x00, 0x01, 0x00, 0x00, /* or [esp], 0x100     */
    0x9d                                      /* popfd               */
  };

  *((guint8 **) (code + 2)) = &block->state;
  gum_x86_writer_put_bytes (gc->code_writer, code, sizeof (code));
  gum_x86_writer_put_jmp_address (gc->code_writer,
      GUM_ADDRESS (gc->instruction->begin));
}

static void
gum_exec_block_write_call_event_code (GumExecBlock * block,
                                      const GumBranchTarget * target,
                                      GumGeneratorContext * gc,
                                      GumCodeContext cc)
{
  GumX86Writer * cw = gc->code_writer;

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);

  gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
      GUM_ADDRESS (gum_exec_ctx_emit_call_event), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->begin),
      GUM_ARG_REGISTER, GUM_REG_XDX);

  gum_exec_block_write_unfollow_check_code (block, gc, cc);
}

static void
gum_exec_block_write_ret_event_code (GumExecBlock * block,
                                     GumGeneratorContext * gc,
                                     GumCodeContext cc)
{
  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_x86_writer_put_call_address_with_aligned_arguments (gc->code_writer,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_exec_ctx_emit_ret_event), 2,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->begin));

  gum_exec_block_write_unfollow_check_code (block, gc, cc);
}

static void
gum_exec_block_write_exec_event_code (GumExecBlock * block,
                                      GumGeneratorContext * gc,
                                      GumCodeContext cc)
{
  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_x86_writer_put_call_address_with_aligned_arguments (gc->code_writer,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_exec_ctx_emit_exec_event), 2,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->begin));

  gum_exec_block_write_unfollow_check_code (block, gc, cc);
}

static void
gum_exec_block_write_block_event_code (GumExecBlock * block,
                                       GumGeneratorContext * gc,
                                       GumCodeContext cc)
{
  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_x86_writer_put_call_address_with_aligned_arguments (gc->code_writer,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_exec_ctx_emit_block_event), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->relocator->input_start),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->relocator->input_cur));

  gum_exec_block_write_unfollow_check_code (block, gc, cc);
}

static void
gum_exec_block_write_unfollow_check_code (GumExecBlock * block,
                                          GumGeneratorContext * gc,
                                          GumCodeContext cc)
{
  GumExecCtx * ctx = block->ctx;
  GumX86Writer * cw = gc->code_writer;
  gconstpointer beach = cw->code + 1;
  GumPrologType opened_prolog;

  if (cc != GUM_CODE_INTERRUPTIBLE)
    return;

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_EAX,
      GUM_ADDRESS (&ctx->state));
  gum_x86_writer_put_cmp_reg_i32 (cw, GUM_REG_EAX,
      GUM_EXEC_CTX_UNFOLLOW_PENDING);
  gum_x86_writer_put_jcc_near_label (cw, X86_INS_JNE, beach, GUM_LIKELY);
  gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
      GUM_ADDRESS (gum_exec_ctx_unfollow), 2,
      GUM_ARG_ADDRESS, GUM_ADDRESS (ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->begin));
  opened_prolog = gc->opened_prolog;
  gum_exec_block_close_prolog (block, gc);
  gc->opened_prolog = opened_prolog;
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&ctx->resume_at));

  gum_x86_writer_put_label (cw, beach);
}

static void
gum_exec_block_invoke_call_probes_for_target (GumExecBlock * block,
                                              gpointer location,
                                              gpointer target_address,
                                              gpointer return_address,
                                              GumCpuContext * cpu_context)
{
  GumStalkerPrivate * priv = block->ctx->stalker->priv;
  GArray * probes;

  gum_spinlock_acquire (&priv->probe_lock);

  probes = (GArray *)
      g_hash_table_lookup (priv->probe_array_by_address, target_address);
  if (probes != NULL)
  {
    GumCallSite call_site;
    gpointer * return_address_slot;
    guint i;

    call_site.block_address = block->real_begin;
    call_site.stack_data = ((gpointer *) block->ctx->app_stack) - 1;
    call_site.cpu_context = cpu_context;

    GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (location);

    return_address_slot = call_site.stack_data;
    *return_address_slot = return_address;
    GUM_CPU_CONTEXT_XSP (cpu_context) = GPOINTER_TO_SIZE (return_address_slot);

    for (i = 0; i != probes->len; i++)
    {
      GumCallProbe * probe = &g_array_index (probes, GumCallProbe, i);

      probe->callback (&call_site, probe->user_data);
    }
  }

  gum_spinlock_release (&priv->probe_lock);
}

static void
gum_exec_block_write_call_probe_code (GumExecBlock * block,
                                      const GumBranchTarget * target,
                                      GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  gboolean skip_probing = FALSE;

  if (!target->is_indirect && target->base == X86_REG_INVALID)
  {
    GumStalkerPrivate * priv = block->ctx->stalker->priv;

    gum_spinlock_acquire (&priv->probe_lock);
    skip_probing = g_hash_table_lookup (priv->probe_array_by_address,
        target->absolute_address) == NULL;
    gum_spinlock_release (&priv->probe_lock);
  }

  if (!skip_probing)
  {
    if (gc->opened_prolog != GUM_PROLOG_NONE)
      gum_exec_block_close_prolog (block, gc);
    gum_exec_block_open_prolog (block, GUM_PROLOG_FULL, gc);

    gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);

    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_invoke_call_probes_for_target), 5,
        GUM_ARG_ADDRESS, GUM_ADDRESS (block),
        GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->begin),
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->end),
        GUM_ARG_REGISTER, GUM_REG_XBX);
  }
}

static void
gum_exec_block_open_prolog (GumExecBlock * block,
                            GumPrologType type,
                            GumGeneratorContext * gc)
{
  if (gc->opened_prolog >= type)
    return;

  /* We don't want to handle this case for performance reasons */
  g_assert (gc->opened_prolog == GUM_PROLOG_NONE);

  gc->opened_prolog = type;
  gc->accumulated_stack_delta = 0;

  gum_exec_ctx_write_prolog (block->ctx, type, gc->code_writer);
}

static void
gum_exec_block_close_prolog (GumExecBlock * block,
                             GumGeneratorContext * gc)
{
  if (gc->opened_prolog == GUM_PROLOG_NONE)
    return;

  gum_exec_ctx_write_epilog (block->ctx, gc->opened_prolog, gc->code_writer);

  gc->accumulated_stack_delta = 0;
  gc->opened_prolog = GUM_PROLOG_NONE;
}

static void
gum_write_segment_prefix (uint8_t segment,
                          GumX86Writer * cw)
{
  switch (segment)
  {
    case X86_REG_INVALID: break;

    case X86_REG_CS: gum_x86_writer_put_u8 (cw, 0x2e); break;
    case X86_REG_SS: gum_x86_writer_put_u8 (cw, 0x36); break;
    case X86_REG_DS: gum_x86_writer_put_u8 (cw, 0x3e); break;
    case X86_REG_ES: gum_x86_writer_put_u8 (cw, 0x26); break;
    case X86_REG_FS: gum_x86_writer_put_u8 (cw, 0x64); break;
    case X86_REG_GS: gum_x86_writer_put_u8 (cw, 0x65); break;

    default:
      g_assert_not_reached ();
      break;
  }
}

static GumCpuReg
gum_cpu_meta_reg_from_real_reg (GumCpuReg reg)
{
  if (reg >= GUM_REG_EAX && reg <= GUM_REG_EDI)
    return (GumCpuReg) (GUM_REG_XAX + reg - GUM_REG_EAX);
  else if (reg >= GUM_REG_RAX && reg <= GUM_REG_RDI)
    return (GumCpuReg) (GUM_REG_XAX + reg - GUM_REG_RAX);
#if GLIB_SIZEOF_VOID_P == 8
  else if (reg >= GUM_REG_R8D && reg <= GUM_REG_R15D)
    return reg;
  else if (reg >= GUM_REG_R8 && reg <= GUM_REG_R15)
    return reg;
#endif
  else if (reg == GUM_REG_RIP)
    return GUM_REG_XIP;
  else if (reg != GUM_REG_NONE)
    g_assert_not_reached ();

  return GUM_REG_NONE;
}

static GumCpuReg
gum_cpu_reg_from_capstone (x86_reg reg)
{
  switch (reg)
  {
    case X86_REG_INVALID: return GUM_REG_NONE;

    case X86_REG_EAX: return GUM_REG_EAX;
    case X86_REG_ECX: return GUM_REG_ECX;
    case X86_REG_EDX: return GUM_REG_EDX;
    case X86_REG_EBX: return GUM_REG_EBX;
    case X86_REG_ESP: return GUM_REG_ESP;
    case X86_REG_EBP: return GUM_REG_EBP;
    case X86_REG_ESI: return GUM_REG_ESI;
    case X86_REG_EDI: return GUM_REG_EDI;
    case X86_REG_R8D: return GUM_REG_R8D;
    case X86_REG_R9D: return GUM_REG_R9D;
    case X86_REG_R10D: return GUM_REG_R10D;
    case X86_REG_R11D: return GUM_REG_R11D;
    case X86_REG_R12D: return GUM_REG_R12D;
    case X86_REG_R13D: return GUM_REG_R13D;
    case X86_REG_R14D: return GUM_REG_R14D;
    case X86_REG_R15D: return GUM_REG_R15D;
    case X86_REG_EIP: return GUM_REG_EIP;

    case X86_REG_RAX: return GUM_REG_RAX;
    case X86_REG_RCX: return GUM_REG_RCX;
    case X86_REG_RDX: return GUM_REG_RDX;
    case X86_REG_RBX: return GUM_REG_RBX;
    case X86_REG_RSP: return GUM_REG_RSP;
    case X86_REG_RBP: return GUM_REG_RBP;
    case X86_REG_RSI: return GUM_REG_RSI;
    case X86_REG_RDI: return GUM_REG_RDI;
    case X86_REG_R8: return GUM_REG_R8;
    case X86_REG_R9: return GUM_REG_R9;
    case X86_REG_R10: return GUM_REG_R10;
    case X86_REG_R11: return GUM_REG_R11;
    case X86_REG_R12: return GUM_REG_R12;
    case X86_REG_R13: return GUM_REG_R13;
    case X86_REG_R14: return GUM_REG_R14;
    case X86_REG_R15: return GUM_REG_R15;
    case X86_REG_RIP: return GUM_REG_RIP;

    default:
      g_assert_not_reached ();
  }
}

static x86_insn
gum_negate_jcc (x86_insn instruction_id)
{
  switch (instruction_id)
  {
    case X86_INS_JA:
      return X86_INS_JBE;
    case X86_INS_JAE:
      return X86_INS_JB;
    case X86_INS_JB:
      return X86_INS_JAE;
    case X86_INS_JBE:
      return X86_INS_JA;
    case X86_INS_JE:
      return X86_INS_JNE;
    case X86_INS_JG:
      return X86_INS_JLE;
    case X86_INS_JGE:
      return X86_INS_JL;
    case X86_INS_JL:
      return X86_INS_JGE;
    case X86_INS_JLE:
      return X86_INS_JG;
    case X86_INS_JNE:
      return X86_INS_JE;
    case X86_INS_JNO:
      return X86_INS_JO;
    case X86_INS_JNP:
      return X86_INS_JP;
    case X86_INS_JNS:
      return X86_INS_JS;
    case X86_INS_JO:
      return X86_INS_JNO;
    case X86_INS_JP:
      return X86_INS_JNP;
    case X86_INS_JS:
      return X86_INS_JNS;
    default:
      g_assert_not_reached ();
  }
}

#if defined (G_OS_WIN32) && GLIB_SIZEOF_VOID_P == 4

static void
enable_hardware_breakpoint (DWORD * dr7_reg, guint index)
{
  /* set both RWn and LENn to 00 */
  *dr7_reg &= ~(0xf << (16 + (2 * index)));

  /* set LE bit */
  *dr7_reg |= 1 << (2 * index);
}

static gpointer
find_system_call_above_us (GumStalker * stalker, gpointer * start_esp)
{
  GumStalkerPrivate * priv = stalker->priv;
  gpointer * top_esp, * cur_esp;
  guint8 call_fs_c0_code[] = { 0x64, 0xff, 0x15, 0xc0, 0x00, 0x00, 0x00 };
  guint8 call_ebp_8_code[] = { 0xff, 0x55, 0x08 };
  guint8 * minimum_address, * maximum_address;

  __asm
  {
    mov eax, fs:[4];
    mov [top_esp], eax;
  }

  if ((guint) ABS (top_esp - start_esp) > priv->page_size)
  {
    top_esp = (gpointer *) ((GPOINTER_TO_SIZE (start_esp) +
        (priv->page_size - 1)) & ~(priv->page_size - 1));
  }

  /* These boundaries are quite artificial... */
  minimum_address = (guint8 *) priv->user32_start + sizeof (call_fs_c0_code);
  maximum_address = (guint8 *) priv->user32_end - 1;

  for (cur_esp = start_esp + 1; cur_esp < top_esp; cur_esp++)
  {
    guint8 * address = (guint8 *) *cur_esp;

    if (address >= minimum_address && address <= maximum_address)
    {
      if (memcmp (address - sizeof (call_fs_c0_code), call_fs_c0_code,
          sizeof (call_fs_c0_code)) == 0
          || memcmp (address - sizeof (call_ebp_8_code), call_ebp_8_code,
          sizeof (call_ebp_8_code)) == 0)
      {
        return address;
      }
    }
  }

  return NULL;
}

static gboolean
gum_stalker_on_exception (GumExceptionDetails * details,
                          gpointer user_data)
{
  GumStalker * self = GUM_STALKER_CAST (user_data);
  GumExecCtx * ctx;
  GumExecBlock * block;
  GumCpuContext * cpu_context = &details->context;
  CONTEXT * context = details->native_context;

  if (details->type != GUM_EXCEPTION_SINGLE_STEP)
    return FALSE;

  ctx = gum_stalker_get_exec_ctx (self);
  if (ctx == NULL)
    return FALSE;

  block = ctx->current_block;

  /*printf ("gum_stalker_handle_exception state=%u %p %08x\n",
      block->state, context->Eip, exception_record->ExceptionCode);*/

  switch (block->state)
  {
    case GUM_EXEC_NORMAL:
    case GUM_EXEC_SINGLE_STEPPING_ON_CALL:
    {
      DWORD instruction_after_call_here;
      DWORD instruction_after_call_above_us;

      block->previous_dr0 = context->Dr0;
      block->previous_dr1 = context->Dr1;
      block->previous_dr2 = context->Dr2;
      block->previous_dr7 = context->Dr7;

      instruction_after_call_here = cpu_context->eip +
          gum_x86_reader_insn_length ((guint8 *) cpu_context->eip);
      context->Dr0 = instruction_after_call_here;
      enable_hardware_breakpoint (&context->Dr7, 0);

      context->Dr1 = (DWORD) self->priv->ki_user_callback_dispatcher_impl;
      enable_hardware_breakpoint (&context->Dr7, 1);

      instruction_after_call_above_us = (DWORD)
          find_system_call_above_us (self, (gpointer *) cpu_context->esp);
      if (instruction_after_call_above_us != 0)
      {
        context->Dr2 = instruction_after_call_above_us;
        enable_hardware_breakpoint (&context->Dr7, 2);
      }

      block->state = GUM_EXEC_SINGLE_STEPPING_THROUGH_CALL;

      break;
    }

    case GUM_EXEC_SINGLE_STEPPING_THROUGH_CALL:
    {
      context->Dr0 = block->previous_dr0;
      context->Dr1 = block->previous_dr1;
      context->Dr2 = block->previous_dr2;
      context->Dr7 = block->previous_dr7;

      gum_exec_ctx_replace_current_block_with (ctx,
          GSIZE_TO_POINTER (cpu_context->eip));
      cpu_context->eip = (DWORD) ctx->resume_at;

      block->state = GUM_EXEC_NORMAL;

      break;
    }

    default:
      g_assert_not_reached ();
  }

  return TRUE;
}

#endif

void
gum_stalker_set_counters_enabled (gboolean enabled)
{
  counters_enabled = enabled;
}

void
gum_stalker_dump_counters (void)
{
  g_printerr ("\n\ntotal_transitions: %u\n", total_transitions);

#if GLIB_SIZEOF_VOID_P == 4 && !defined (HAVE_QNX)
  GUM_PRINT_ENTRYGATE_COUNTER (sysenter_slow_path);

  g_printerr ("\n");
#endif

  GUM_PRINT_ENTRYGATE_COUNTER (call_imm);
  GUM_PRINT_ENTRYGATE_COUNTER (call_reg);
  GUM_PRINT_ENTRYGATE_COUNTER (call_mem);
  GUM_PRINT_ENTRYGATE_COUNTER (post_call_invoke);
  GUM_PRINT_ENTRYGATE_COUNTER (ret_slow_path);

  g_printerr ("\n");

  GUM_PRINT_ENTRYGATE_COUNTER (jmp_imm);
  GUM_PRINT_ENTRYGATE_COUNTER (jmp_mem);
  GUM_PRINT_ENTRYGATE_COUNTER (jmp_reg);

  g_printerr ("\n");

  GUM_PRINT_ENTRYGATE_COUNTER (jmp_cond_imm);
  GUM_PRINT_ENTRYGATE_COUNTER (jmp_cond_mem);
  GUM_PRINT_ENTRYGATE_COUNTER (jmp_cond_reg);
  GUM_PRINT_ENTRYGATE_COUNTER (jmp_cond_jcxz);

  g_printerr ("\n");

  GUM_PRINT_ENTRYGATE_COUNTER (jmp_continuation);
}
