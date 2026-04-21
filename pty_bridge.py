import sys
import os
import tty
import termios
import select

if len(sys.argv) != 2:
    print(f"Usage: {sys.argv[0]} /dev/pts/X")
    sys.exit(1)

pty_path = sys.argv[1]

# Open PTY
fd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY)

# Save terminal settings
old_settings = termios.tcgetattr(sys.stdin)

try:
    tty.setraw(sys.stdin.fileno())

    while True:
        r, _, _ = select.select([sys.stdin, fd], [], [])

        if sys.stdin in r:
            data = os.read(sys.stdin.fileno(), 1024)
            if not data:
                break
            os.write(fd, data)

        if fd in r:
            data = os.read(fd, 1024)
            if not data:
                break
            os.write(sys.stdout.fileno(), data)

finally:
    termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)
    os.close(fd)
