#!/bin/bash
width=1920
height=1080

# Need to customize ip and port
# IMPORTANT: Change server_ip to your machine's public IP address
ip="143.89.79.104"
port="8888"

# Modify this if you want to disable MAE
enable_mae=1
disable_frame_drop=1

usage() {
	echo "[Usage] $0 [-i <video_name>] [-p <program_name>]" 1>&2;
	echo "[Usage] The optional programs contain: gen_send_video and send_and_recv"
	echo "[Usage] The default size is 1920x1080, can modified by [-s <{width}x{height}]"
	echo "[Usage] For example: $0 -i video_0a86 -p send_and_recv -s 1280x720"
	exit 1;
}

while getopts ":i:p:s:" opt; do
  case "${opt}" in
    i)
      video_name=${OPTARG}
      ;;
    p)
      p=${OPTARG}
      if [ $p == "send_and_recv" ] || [ $p == "gen_send_video" ]; then
        run_program=${p}
      else
        usage
      fi
      ;;
    s)
      s=${OPTARG}
      s=(${s//x/ })
      width=${s[0]}
      height=${s[1]}
      ;;
    *)
      usage
      ;;
  esac
done
shift $((OPTIND-1))

if [ -z "${video_name}" ] || [ -z "${run_program}" ]; then
    usage
fi

if [ $run_program == "gen_send_video" ]; then
	python3 process_video_qrcode.py --option=gen_send_video --data=$video_name --height=$height --width=$width
elif [ $run_program == "send_and_recv" ]; then
  # Run sender and receiver
  python3 process_video_qrcode.py --option=send_and_recv --data=$video_name --height=$height\
                                  --width=$width --server_ip=$ip --server_port=$port\
                                  --enable_mae=$enable_mae --disable_frame_drop=$disable_frame_drop
  # Generate result figures
  python3 process_video_qrcode.py --option=show_fig --data=$video_name
fi