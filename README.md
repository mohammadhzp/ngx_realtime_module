# ngx_realtime_module

An Nginx module for **real-time monitoring of active HTTP requests**. Unlike standard access logs that are written after a request completes, this module emits log entries at configurable intervals *during* request processing -- giving you visibility into ongoing file downloads, streaming responses, and long-lived connections before they finish.

## How It Works

The module hooks into Nginx's request processing pipeline and sets up a recurring timer for each monitored request. At every interval tick, it evaluates a user-defined log format string (which can include both standard Nginx variables and module-specific variables) and sends the result to one or both logging backends:

1. **Nginx error log** -- writes to the standard `error_log` destination.
2. **Syslog** -- sends messages to a remote syslog server with batched, queue-based delivery.

A final log entry is always emitted when the request completes, regardless of the interval timer.

```
 Client        Nginx + realtime module           Syslog / error_log
   |                    |                                |
   |--- GET /file ----->|                                |
   |                    |--[interval tick]-- log ------->|
   |<--- chunk ---------|                                |
   |                    |--[interval tick]-- log ------->|
   |<--- chunk ---------|                                |
   |                    |--[interval tick]-- log ------->|
   |<--- last chunk ----|                                |
   |                    |--[request done]--- log ------->|
   |                    |                                |
```

By default the module registers at the **pre-content phase**. If your use case requires it, you can switch to the content phase with `realtime_monitor_from_content_phase`. The module only activates for requests that reach the configured phase -- if a request is rejected earlier (e.g., by access controls), no monitoring overhead is added.

## Compatibility

- Nginx >= **1.18.0**
- Tested on recent Linux

## Build

Build as a static or dynamic Nginx module:

```bash
# Static module
./configure --add-module=/path/to/ngx_realtime_module

# Dynamic module
./configure --add-dynamic-module=/path/to/ngx_realtime_module
```

No external library dependencies are required.

## Directives

### `http {}` block

| Directive | Default | Description |
|---|---|---|
| `realtime_enabled` | `off` | Master switch to enable the module globally. |
| `realtime_syslog_info` | -- | Syslog destination. Uses the same format as the `error_log` directive (e.g., `syslog:server=127.0.0.1:514`). |
| `realtime_monitor_from_content_phase` | `off` | When `on`, the module registers at the content phase instead of pre-content. See [Nginx phases](https://nginx.org/en/docs/dev/development_guide.html#http_phases) for the difference. |
| `realtime_skip_failed_after` | `10` | Drop a queued syslog message after this many consecutive send failures. |
| `realtime_syslog_send_batch_size` | `5` | Number of queued messages sent to the syslog backend per processing event. |
| `realtime_syslog_queue_interval` | `0.3s` | Time between syslog queue processing events. |

### `location {}` block

| Directive | Default | Description |
|---|---|---|
| `realtime_monitor` | `off` | Enable real-time monitoring for this location. |
| `realtime_format` | -- | Log format string. Supports standard Nginx variables and the module variables listed below. |
| `realtime_interval` | `5s` | Time between log emissions for each active request. Minimum `100ms`. |
| `realtime_log_level` | `alert` | Nginx log level used when writing to `error_log`. Valid values: `emerg`, `alert`, `crit`, `error`, `warn`, `notice`, `info`, `debug`. |
| `realtime_error_log` | `off` | Send interval log entries to the Nginx `error_log`. |
| `realtime_syslog` | `on`* | Send interval log entries to syslog. *Enabled by default only when `realtime_syslog_info` is configured. |
| `realtime_error_log_use_cycle` | `off` | Use the global cycle log object instead of the per-connection log object. |

> `realtime_format` can also be set at the `http {}` and `server {}` levels and will be inherited by locations.

## Variables

| Variable | Description |
|---|---|
| `$realtime_body_bytes_sent` | Bytes sent to the client since the **last** interval tick. |
| `$realtime_time_elapsed` | Seconds elapsed since the last interval tick (e.g., `2.003`). |
| `$realtime_is_fresh` | `YES` on the first interval tick of a connection, `NO` on subsequent ticks. |
| `$realtime_request_completion` | `OK` when the request has finished; empty otherwise. Not the same as the built-in `$request_completion`. |
| `$realtime_interval` | The configured interval value (in milliseconds), exposed as a variable for logging convenience. |
| `$realtime_queue_size` | Current depth of the syslog send queue (excludes the item being processed). |

All module variables can be combined freely with standard Nginx variables (`$remote_addr`, `$request_uri`, `$arg_*`, etc.) inside `realtime_format`.

## Example Configuration

```nginx
http {
    realtime_enabled                    on;
    realtime_monitor_from_content_phase on;
    realtime_log_level                  alert;
    realtime_syslog_info                syslog:server=127.0.0.1:5145 error;
    realtime_syslog_queue_interval      3s;
    realtime_syslog_send_batch_size     2;

    server {
        listen 8080;

        location /download/ {
            default_type application/octet-stream;

            realtime_monitor   on;
            realtime_error_log off;
            realtime_syslog    on;
            realtime_interval  2s;

            realtime_format
                'uid=${arg_uid}'
                '|interval=${realtime_interval}'
                '|bytes_sent=${realtime_body_bytes_sent}'
                '|elapsed=${realtime_time_elapsed}'
                '|completed=${realtime_request_completion}'
                '|fresh=${realtime_is_fresh}'
                '|queue=${realtime_queue_size}';
        }
    }
}
```

With this configuration, every 2 seconds each active request under `/download/` produces a log entry like:

```
uid=42|interval=2000|bytes_sent=1048576|elapsed=2.001|completed=|fresh=NO|queue=0
```

When the download finishes, a final entry is emitted with `completed=OK`.

## Sample Syslog Backend

A minimal Python syslog receiver is included at `util/backend.py`. It listens for UDP datagrams on `127.0.0.1:5145` (matching the example configuration above), parses each log entry, and prints it to stdout.

```bash
python util/backend.py
```

Output:

```
('127.0.0.1', 54321): {'uid': '42', 'ri': '2000', 'bbs': '1048576', 'rte': '2.001', 'rrc': '', 'fresh': 'NO', 'queue_size': '0'}
```

No third-party dependencies are needed -- the script uses only the Python standard library (`asyncio`, `socket`). Stop it with `Ctrl+C`.

## Development

See [DEVELOP.md](DEVELOP.md) for instructions on setting up a local debug Nginx build, compiling with or without the module, and running it end-to-end.
