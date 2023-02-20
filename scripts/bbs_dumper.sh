#/bin/sh

# bbs_dumper
# (C) Copyright 2023 Naveen Albert

ps -aux | grep "lbbs" | grep -v "grep"

bbspid=`cat /var/run/lbbs/bbs.pid`
printf "BBS PID: %d\n" $bbspid

ensure_gdb_installed() {
	# For some reason, using which is not sufficient and will lead to things like: /usr/bin/gdb does not support python
	# That's because gdb isn't really installed.
	# Use a technique aside from which/path/binary detection to see if we find something we expect:
	helplines=`gdb --help | grep "GDB manual" | wc -l`
	if [ "$helplines" != "1" ]; then
		printf "GDB does not appear to be currently installed, trying to install it now...\n"
		apt-get install -y gdb
	fi
}

if [ "$1" = "pid" ]; then
	echo $bbspid
elif [ "$1" = "term" ]; then
	kill -9 $bbspid
elif [ "$1" = "quit" ]; then
	kill -3 $bbspid
elif [ "$1" = "postdump" ]; then
	ensure_gdb_installed
	gdb /usr/sbin/lbbs core -ex "thread apply all bt full" -ex "quit" > full.txt
	printf "Backtrace saved to full.txt\n"
elif [ "$1" = "livedump" ]; then
	ensure_gdb_installed
	gdb /usr/sbin/lbbs --batch -q -p $bbspid -ex 'thread apply all bt full' -ex 'quit' > full.txt
	printf "Backtrace saved to full.txt\n"
else
	echo "Invalid command!"
	exit 1
fi
