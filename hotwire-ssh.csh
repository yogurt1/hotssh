[ -x //usr/bin/id ] || exit
[ `//usr/bin/id -u` -gt 100 ] && alias ssh hotwire-ssh --bg
