# cpp-httplib

`httplib.h` from [v0.54.1](https://github.com/yhirose/cpp-httplib/releases/tag/v0.54.1), vendored with its MIT license. The local `Response::ranges_applied` flag keeps an already ranged upstream response from being validated or sliced a second time by the gateway. Default server behavior is unchanged; `TestRemoteProxyStreamsAndKeepsCredentialsHostOwned` covers the relay path.

Only the native web adapter includes this dependency. The build disables exceptions and the default user agent, and does not enable its TLS/compression integrations. TLS terminates at the optional trusted local proxy. The existing libcurl client remains the outbound transport.

The relay also passes a bounded upload provider to `open_stream`, using the
client's existing content-provider writer. This lets the gateway forward an
upload with socket backpressure while retaining the pull-based response stream.
