import sys
import os
import tty
import termios
import select
import argparse
import signal


def main():
    parser = argparse.ArgumentParser(
        description="Bridge host stdin/stdout to a guest PTY"
    )
    parser.add_argument("pty", help="Path to PTY slave (e.g. /dev/pts/8)")
    parser.add_argument(
        "--raw", action="store_true", help="Put local stdin into raw mode"
    )
    args = parser.parse_args()

    pty_path = args.pty

    # Open PTY
    try:
        fd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY)
    except Exception as e:
        print(f"Error: unable to open {pty_path}: {e}", file=sys.stderr)
        return 2

    old_settings = None
    stdin_fd = sys.stdin.fileno()

    # Only attempt to change terminal settings if stdin is a tty
    if args.raw and os.isatty(stdin_fd):
        try:
            old_settings = termios.tcgetattr(stdin_fd)
            tty.setraw(stdin_fd)
        except Exception as e:
            print(f"Warning: unable to set raw mode on stdin: {e}", file=sys.stderr)
            old_settings = None

    def cleanup(signum=None, frame=None):
        # Restore terminal settings if we changed them
        try:
            if old_settings is not None:
                termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_settings)
        except Exception:
            pass
        try:
            os.close(fd)
        except Exception:
            pass
        # If called from signal handler, exit explicitly
        if signum is not None:
            sys.exit(0)

    # Install signal handlers to ensure terminal is restored
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)
    try:
        signal.signal(signal.SIGHUP, cleanup)
    except Exception:
        pass

    try:
        while True:
            r, _, _ = select.select([sys.stdin, fd], [], [])

            if sys.stdin in r:
                data = os.read(stdin_fd, 1024)
                if not data:
                    break
                os.write(fd, data)

            if fd in r:
                data = os.read(fd, 1024)
                if not data:
                    break
                os.write(sys.stdout.fileno(), data)
    except KeyboardInterrupt:
        # Allow Ctrl-C to exit when not in raw mode; if in raw mode SIGINT handled above
        pass
    finally:
        cleanup()


if __name__ == "__main__":
    main()
