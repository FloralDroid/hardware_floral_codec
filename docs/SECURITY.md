# Codec2 service security modes

The component owns `config/seccomp/`, `service/ServiceSandbox.cpp`, and its
security-property SELinux rules. Consuming boards include
`hardware/floral/codec/sepolicy/BoardConfig.mk`; no generic app grant is needed
for this service sandbox.

`androidboot.floral_codec_seccomp` becomes the read-only
`ro.boot.floral_codec_seccomp` property:

| Value | Behavior |
| --- | --- |
| `0` | Explicit development opt-out: do not query SELinux, allocate a jail, or install an extra filter. |
| `1` | Require enforcing SELinux and install the mandatory component policy. Failure prevents service registration. |
| unset | Enable with confirmed Enforcing; skip only with confirmed Permissive or disabled/unmounted SELinux. Unknown state or query failure prevents service registration. |
| anything else | Refuse to start the service. |

State detection distinguishes Enforcing, Permissive, confirmed disabled/unmounted,
and Unknown/Error. A denied or failed status query is not evidence of disabled
SELinux. The component gets only `selinuxfs:file { open read }` for this query,
not enforcement-control permissions or a claim of an enforce-file-only grant.
An explicit `0` remains usable in development even when the query would fail.

Updated Lupine supplies `1` only when both SELinux is `enforcing` and its seccomp
setting is `allowlist`. It supplies `0` for development configurations that
disable either mechanism. Older launchers must supply an explicit `0` when
developing with enforcing SELinux but no component seccomp. The flag does not
remove inherited host filters or change Android's other services' sandboxes.

Installation happens in `main`, before Binder pools, codec-store creation and
driver probing. Direct `libminijail` calls set no-new-privileges and synchronize
the filter across threads. There is **no log-only seccomp mode**, even with a
debug kernel/firmware. The mandatory policy cannot fail open or be replaced by
an optional permissive vendor extension. The post-check also requires active
filter mode and no-new-privileges.

ARM64 and x86_64 install their own architecture entry point, sharing a syscall
list for Binder, V4L2/DRM/DMA-BUF, worker threads and crash reporting. Argument
checks reject W+X mappings, non-thread clone and non-UNIX sockets. Exec, mount,
ptrace, BPF and namespace-changing syscalls are not allowed. This is **not** a
path or ioctl-command whitelist: SELinux/DAC and host device exposure must be
audited separately. Existing device-level FMQ/graphics adaptation rules are not
migrated by this change.

No codec algorithm, queue, driver, firmware, `dev/` contents or init failure
policy is changed. A sandbox rejection is local to the service; this code adds
no host reboot or panic action. It cannot make buggy kernel/firmware code safe.

## Validation and acceptance

Host mode-selection and state-reader tests are in `tests/SandboxModeTest.cpp`
and `tests/SelinuxStateTest.cpp`, including query-error fail-closed cases and
explicit opt-out without a query. Offline Android 12
minijail parser/BPF checks cover both syscall-number tables, and a mocked API
test checks the real startup source's opt-out and installation ordering.
These do not replace a target build or a hardware codec round trip.

Before production acceptance, rebuild the component and policies, then verify:

- Hardware rendering + SELinux Enforcing, actual codec service domain unchanged.
- The component filter is added (filter count increases over the inherited
  host count), NNP is 1, and a forbidden call is denied in a disposable probe.
- Decode, ByteBuffer encode and Surface encode work on the real driver.
- Explicit opt-out skips this filter in disabled/permissive SELinux and
  enforcing+no-seccomp development configurations.

Do not disable production enforcement automatically to make a codec test pass.
