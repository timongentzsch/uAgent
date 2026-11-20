# cpp-httplib

`httplib.h`, unmodified except for one socket-liveness analyzer annotation and v0.62.0's condition for starting a thread (one per queued connection, not only when no thread is idle), from [v0.54.1](https://github.com/yhirose/cpp-httplib/releases/tag/v0.54.1), vendored with its MIT license.

Only the native web adapter includes this dependency. The build disables exceptions and the default user agent, and does not enable its TLS/compression integrations. TLS terminates at the optional trusted local proxy. The existing libcurl client remains the outbound transport.
