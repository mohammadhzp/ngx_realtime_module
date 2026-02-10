# Development Guide

A local development setup that lets you build a debug Nginx, compile the module into it, and run it -- all from the project root directory.

> All scripts below **must** be run from the project root.

## Prerequisites

- GCC (or another C compiler)
- Make
- curl
- Python 3 (for the sample syslog backend)

## 1. Install Nginx

Run the installer script to download, compile, and install a debug-enabled Nginx into `build/nginx/`:

```bash
util/installer
```

This will:

1. Create `vendor/` and `build/` directories.
2. Download the Nginx source tarball (default version: **1.28.2**) into `vendor/`.
3. Configure, compile, and install Nginx with `--with-debug` into `build/nginx/`.
4. Symlink `util/nginx.conf` into `build/nginx/conf/nginx.conf`.

To change the Nginx version, edit the `NGINX_VERSION` variable at the top of `util/installer` before running it.

To start fresh, remove everything and re-install:

```bash
util/installer clean
util/installer
```

## 2. Compile

Two compile scripts are provided so you can quickly switch between builds with and without the module.

### With the realtime module

```bash
util/compile
```

Configures and builds Nginx with `--add-module` pointing to the project root, then installs into `build/nginx/`.

### Without the realtime module

```bash
util/compile_no_realtime
```

Same as above but without the module -- useful for verifying baseline behavior or isolating issues.

Both scripts build with debug symbols (`-g -O0`) enabled.

## 3. Configure

A ready-to-use configuration file is provided at `util/nginx.conf`. The installer automatically symlinks it, but if you need to copy it manually:

```bash
cp util/nginx.conf build/nginx/conf/nginx.conf
```

Before running, edit the `working_directory` directive to match your project path:

```nginx
working_directory  /absolute/path/to/ngx_realtime_module/build/nginx/dumps;
```

Make sure the target directory exists:

```bash
mkdir -p build/nginx/dumps
```

## 4. Run

The sample config serves a file called `final.zip` from the document root. Place any file there so requests don't return a 404:

```bash
cp /path/to/any/file build/nginx/html/final.zip
```

Start Nginx (runs in the foreground due to `daemon off;` in the config):

```bash
build/nginx/sbin/nginx
```

In a separate terminal, start the sample syslog backend to receive log entries:

```bash
python util/backend.py
```

Nginx listens on port **7779**. Send a test request:

```bash
curl http://localhost:7779/
```

The backend terminal will print parsed log entries every 2 seconds as the request is served.

Since the config uses `daemon off;`, press `Ctrl+C` in the Nginx terminal to stop it.
