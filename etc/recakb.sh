#!/bin/bash

ADAPTOR=0
FRONTEND_S=0
FRONTEND_T=1
CONFIG_S=/app-config/dvbv5_channels_isdbs.conf
CONFIG_T=/app-config/dvbv5_channels_isdbt.conf

while getopts "a:" opt; do
	case "$opt" in
		a)
			ADAPTOR=$OPTARG
			;;
		\?)
			echo "Error: 無効なオプションが指定されました。" >&2
			exit 1
			;;
	esac
done

shift $((OPTIND - 1))

CHANNEL=$1

if [ -z "$CHANNEL" ]; then
	echo "Error: チャンネルが指定されていません。" >&2
	exit 1
fi

if [[ "$CHANNEL" =~ ^CS ]]; then
	# CSの場合
	exec dvbv5-zap -a $ADAPTOR -f $FRONTEND_S -c $CONFIG_S -r -P $CHANNEL
elif [[ "$CHANNEL" =~ ^BS ]]; then
	# BSの場合
	exec dvbv5-zap -a $ADAPTOR -f $FRONTEND_S -c $CONFIG_S -r -P $CHANNEL
elif [[ "$CHANNEL" =~ ^C[0-9]+ ]]; then
	# CATVの場合
	exec dvbv5-zap -a $ADAPTOR -f $FRONTEND_T -c $CONFIG_T -r -P $CHANNEL
else
	# 地デジの場合
	exec dvbv5-zap -a $ADAPTOR -f $FRONTEND_T -c $CONFIG_T -r -P $CHANNEL
fi