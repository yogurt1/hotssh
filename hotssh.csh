[ -x //usr/bin/id ] || exit
[ `//usr/bin/id -u` -gt 100 ] && alias ssh hotssh --bg
