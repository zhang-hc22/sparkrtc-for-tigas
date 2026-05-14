#!/bin/bash
set -e

script_dir="$(cd "$(dirname "$0")" && pwd)"
workspace_root="$(cd "$script_dir/../../.." && pwd)"

width=1920
height=1080

# Need to customize ip and port
# IMPORTANT: Change server_ip to your machine's public IP address
ip="${SPARKRTC_SERVER_IP:-127.0.0.1}"
port="${SPARKRTC_SERVER_PORT:-8888}"

# Modify this if you want to disable MAE
enable_mae="${SPARKRTC_ENABLE_MAE:-1}"
enable_adaptive_frame_drop="${SPARKRTC_ENABLE_ADAPTIVE_FRAME_DROP:-0}"
disable_frame_drop="${SPARKRTC_DISABLE_FRAME_DROP:-1}"
sender_wrapper="${SPARKRTC_SENDER_WRAPPER:-}"
receiver_wrapper="${SPARKRTC_RECEIVER_WRAPPER:-}"
server_wrapper="${SPARKRTC_SERVER_WRAPPER:-}"
start_local_server="${SPARKRTC_START_LOCAL_SERVER:-1}"
tmp_dir="${SPARKRTC_TMPDIR:-$workspace_root/.tmp}"
skip_vmaf="${SPARKRTC_SKIP_VMAF:-0}"
skip_figures="${SPARKRTC_SKIP_FIGURES:-0}"

mkdir -p "$tmp_dir"
export TMPDIR="$tmp_dir"

usage() {
	echo "[Usage] $0 [-i <video_name>] [-p <program_name>]" 1>&2;
  echo "[Usage] The optional programs contain: gen_send_video, send_and_recv and analyze_only"
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
      if [ "$p" == "send_and_recv" ] || [ "$p" == "gen_send_video" ] || [ "$p" == "analyze_only" ]; then
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

if [ "$run_program" == "gen_send_video" ]; then
	python3 process_video_qrcode.py --option=gen_send_video --data="$video_name" --height="$height" --width="$width"
elif [ "$run_program" == "send_and_recv" ]; then
  # Run sender and receiver
  python3 process_video_qrcode.py --option=send_and_recv --data="$video_name" --height="$height"\
                                  --width="$width" --server_ip="$ip" --server_port="$port"\
                                  --enable_mae="$enable_mae" --enable_adaptive_frame_drop="$enable_adaptive_frame_drop"\
                                  --disable_frame_drop="$disable_frame_drop"\
                                  --sender_wrapper="$sender_wrapper" --receiver_wrapper="$receiver_wrapper"\
                                  --server_wrapper="$server_wrapper" --start_local_server="$start_local_server"\
                                  --skip_vmaf="$skip_vmaf" --skip_figures="$skip_figures"
  if [ "$skip_figures" != "1" ]; then
    python3 process_video_qrcode.py --option=show_fig --data="$video_name"
  fi
elif [ "$run_program" == "analyze_only" ]; then
  python3 process_video_qrcode.py --option=analyze_only --data="$video_name" --height="$height"\
                                  --width="$width" --skip_vmaf="$skip_vmaf" --skip_figures="$skip_figures"
fi