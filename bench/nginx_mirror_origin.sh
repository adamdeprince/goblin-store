#!/usr/bin/env bash
# Run an isolated, high-throughput nginx origin for the mirror-cache benchmarks.

set -Eeuo pipefail

NGINX_BIN=${NGINX_BIN:-/usr/sbin/nginx}
DIRECTORY=
ACCESS_LOG=
WORK_DIR=
PORT=18000
WORKERS=24

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 2
}

while (( $# )); do
    case $1 in
        --directory) DIRECTORY=${2:?}; shift 2 ;;
        --access-log) ACCESS_LOG=${2:?}; shift 2 ;;
        --work-dir) WORK_DIR=${2:?}; shift 2 ;;
        --port) PORT=${2:?}; shift 2 ;;
        --workers) WORKERS=${2:?}; shift 2 ;;
        --nginx) NGINX_BIN=${2:?}; shift 2 ;;
        --help|-h)
            printf 'usage: %s --directory DIR --access-log FILE --work-dir DIR [--port N] [--workers N] [--nginx FILE]\n' "$0"
            exit 0
            ;;
        *) die "unknown option: $1" ;;
    esac
done

[[ -n $DIRECTORY ]] || die '--directory is required'
[[ -n $ACCESS_LOG ]] || die '--access-log is required'
[[ -n $WORK_DIR ]] || die '--work-dir is required'
[[ -d $DIRECTORY ]] || die "not a directory: $DIRECTORY"
[[ -x $NGINX_BIN ]] || die "nginx is not executable: $NGINX_BIN"
[[ $DIRECTORY == /* && $ACCESS_LOG == /* && $WORK_DIR == /* ]] ||
    die 'directory, access log, and work directory must be absolute paths'
[[ $PORT =~ ^[0-9]+$ ]] && (( PORT > 0 && PORT <= 65535 )) || die 'invalid port'
[[ $WORKERS =~ ^[0-9]+$ ]] && (( WORKERS > 0 && WORKERS <= 256 )) || die 'invalid worker count'
[[ $DIRECTORY != *[$'\t\r\n ;{}']* && $ACCESS_LOG != *[$'\t\r\n ;{}']* &&
   $WORK_DIR != *[$'\t\r\n ;{}']* ]] || die 'nginx paths contain unsupported characters'

mkdir -p "$WORK_DIR" "$(dirname "$ACCESS_LOG")"
: > "$ACCESS_LOG"

CONFIG=$WORK_DIR/nginx.conf
cat > "$CONFIG" <<EOF
pid $WORK_DIR/nginx.pid;
error_log $WORK_DIR/error.log warn;
worker_processes $WORKERS;
worker_rlimit_nofile 65536;

events {
    use epoll;
    worker_connections 8192;
    multi_accept on;
}

http {
    include /etc/nginx/mime.types;
    default_type application/octet-stream;

    log_format benchmark '\$msec\t\$request_method\t\$request_uri\t\$status\t\$body_bytes_sent\t\$request_time';
    access_log $ACCESS_LOG benchmark buffer=1m flush=1s;

    sendfile on;
    sendfile_max_chunk 8m;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 300s;
    keepalive_requests 1000000;
    send_timeout 300s;
    reset_timedout_connection on;

    open_file_cache max=60000 inactive=10m;
    open_file_cache_valid 10m;
    open_file_cache_min_uses 1;
    open_file_cache_errors on;

    server {
        listen 127.0.0.1:$PORT reuseport backlog=8192;
        server_name _;
        root $DIRECTORY;
        autoindex off;

        location / {
            limit_except GET HEAD { deny all; }
            try_files \$uri =404;
            add_header Cache-Control "public, max-age=604800, immutable" always;
        }
    }
}
EOF

BOOTSTRAP_ALERT='could not open error log file: open() "/var/log/nginx/error.log" failed (13: Permission denied)'
"$NGINX_BIN" -t -q -p "$WORK_DIR/" -c "$CONFIG" \
    2> >(grep -Fv "$BOOTSTRAP_ALERT" >&2)
exec "$NGINX_BIN" -p "$WORK_DIR/" -c "$CONFIG" -g 'daemon off; master_process on;' \
    2> >(grep -Fv "$BOOTSTRAP_ALERT" >&2)
