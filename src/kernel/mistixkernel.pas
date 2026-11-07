unit MistixKernel;

{$mode objfpc}
{$H-}
{$ifdef MISTIX_DEBUG_CHECKS}
{$R+}
{$Q+}
{$else}
{$R-}
{$Q-}
{$endif}
{$S-}
{$I-}
{$ALIGN 8}
{$ASMMODE intel}

interface

implementation

{$I mistix_types.inc}
{$I mistix_console.inc}
{$I mistix_cpu.inc}
{$I mistix_memory.inc}
{$I mistix_threads.inc}
{$I mistix_capability.inc}
{$I mistix_ipc.inc}
{$I mistix_notification.inc}
{$I mistix_memory_object.inc}
{$I mistix_address_space.inc}
{$I mistix_cow.inc}
{$I mistix_block.inc}
{$I mistix_ext2.inc}
{$I mistix_mount.inc}
{$I mistix_net.inc}
{$I mistix_vfs.inc}
{$I mistix_process.inc}
{$I mistix_shm.inc}
{$I mistix_syscall.inc}
{$I mistix_timer.inc}
{$I mistix_trap.inc}
{$I mistix_main.inc}
{$I mistix_runtime.inc}

end.
