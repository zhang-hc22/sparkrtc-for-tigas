import os
import qrcode
import cv2
import argparse
import matplotlib.pyplot as plt
import numpy as np
import subprocess
import signal
import sys
import time
import psutil
import json

from concurrent.futures import ThreadPoolExecutor
from math import log10, sqrt

ffmpeg_path = "ffmpeg"
fps = 30

## Generate qrcode and overlay to video
def gen_qrcode_pic(num, data_dir):
    qr = qrcode.QRCode(
        version=1,
        error_correction=qrcode.constants.ERROR_CORRECT_H,
        box_size=6,
        border=1,
    )
    qr.add_data(str(num))
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white")
    img.save(data_dir + "/qrcode_"+str(num)+".png")

def gen_qrcode(cfg, num):
    data_dir = "../qrcode/"+cfg.data
    os.system("rm -rf " + data_dir)
    os.system("mkdir -p " + data_dir)

    for i in range(1, num+1):
        gen_qrcode_pic(i, data_dir)
    ffmpeg_command = ffmpeg_path + " -f image2 -r "+ str(fps) +" -i " + data_dir + "/qrcode_%d.png -pix_fmt yuv420p " + data_dir + "/qrcode_output.yuv -y"

    os.system(ffmpeg_command)

def overlay_qrcode_to_video(cfg):
    video_path = "../data/" + cfg.data + ".yuv"

    if not os.path.exists(video_path):
        mp4_video_path = "../data/" + cfg.data + ".mp4"
        if not os.path.exists(mp4_video_path):
            sys.exit(f"Error: {video_path} not exsist!")
        else:
            os.system(f"ffmpeg -i {mp4_video_path} {video_path} -y")

    if not os.path.exists(video_path):
        sys.exit(f"Error: {video_path} not exsist!")

    file_stats = os.stat(video_path)
    i420_frame_size = 3 * cfg.width * cfg.height / 2
    frame_count = int(file_stats.st_size / i420_frame_size)

    print(f'Frame count: {frame_count}')

    send_pic = "../send/" + cfg.data

    if os.path.exists(send_pic):
    return

    gen_qrcode(cfg, frame_count)
    os.system("mkdir -p "+send_pic)

    qrcode_path = "../qrcode/" + cfg.data + "/qrcode_output.yuv"
    output_path = "../data/" + cfg.data + "_qrcode.yuv"
    ffmpeg_command = ffmpeg_path + " -s " + str(cfg.width) + "x" + str(cfg.height) + " -i " +\
                        video_path + " -s 138x138 -i " +\
                        qrcode_path + " -s 138x138 -i " +\
                        qrcode_path + " -s 138x138 -i " +\
                        qrcode_path + " -filter_complex \"[0][1]overlay=30:30[v1]; " +\
                                        "[v1][2]overlay=960:30[v2]; " +\
                                        "[v2][3]overlay=1700:30[v3]\" -map \"[v3]\" " +\
                        output_path + " -y"
    print(ffmpeg_command)
    os.system(ffmpeg_command)

    ffmpeg_command = ffmpeg_path + " -s " + str(cfg.width) + "x" + str(cfg.height) + " -i " +\
                        output_path + " ../send/"+ cfg.data + "/frame%d.png -y"
    os.system(ffmpeg_command)

## Decode received video
def scan_qrcode_each(png_path, pre_send_index):
    if os.path.exists(png_path):
        image = cv2.imread(png_path)
        detector = cv2.wechat_qrcode_WeChatQRCode('detect.prototxt','detect.caffemodel', 'sr.prototxt','sr.caffemodel')
        res, _ = detector.detectAndDecode(image)
        if len(res) > 0:
            return res[0]
        else:
            return pre_send_index
    else:
        sys.exit(f"Error: {png_path} not exsist!")

def scan_qrcode_fast(recv_raw_frames_dir, received_frame_cnt):
    drop_frames_index = []
    receive_correspoding_send_index = []
    pre_send_index = 0

    for i in range(1, received_frame_cnt + 1):
        png_path = recv_raw_frames_dir + "frame" + str(i) + ".png"
        send_index = int(scan_qrcode_each(png_path, pre_send_index))
        if pre_send_index + 1 < send_index:
            drop_frames_index.extend(range(pre_send_index + 1, send_index))
        receive_correspoding_send_index.append(send_index)
        pre_send_index = send_index

    return drop_frames_index, receive_correspoding_send_index

def write_data_to_file(data_lists, file):
    with open(file, 'w') as f_file:
        data_len = min([len(x) for x in data_lists])
        for i in range(data_len):
            content = ""
            for data in data_lists:
                content = content + str(data[i]) + ","
            content += "\n"
            f_file.write(content)

def read_data_from_file(file, count, data_lists, seperator, strict_mode = True):
    lines = open(file,'r').read().split('\n')
    for line in lines:
        line = line.split(seperator)
        if strict_mode:
            if len(line) != count:
                continue
            for i in range(count):
                if line[i].isdigit():
                    data_lists[i].append(int(line[i]))
        else:
            if len(line) < count:
                continue
            for i in range(count):
                data_lists[i].append(line[i])

def extract_rate_and_framesize(recv_dir):
    send_log_file = recv_dir + "send.log"
    rate_stamp_file = recv_dir + "rate_timestamp.log"
    frame_size_stamp_file = recv_dir + "frame_size_original_timestamp.log"

    os.system("rm -rf " + rate_stamp_file)
    os.system("rm -rf " + frame_size_stamp_file)

    extract_rate_command = "grep \"Send Statistics SetRates\" " + send_log_file + " | awk \'{print $13, $8}\' > " + rate_stamp_file
    extract_frame_size_command = "grep \"Send Statistics Send Frame Size\" " + send_log_file + " | awk \'{print $10, $7}\' > " + frame_size_stamp_file

    os.system(extract_rate_command)
    os.system(extract_frame_size_command)

    rate_time = []
    rate = []
    read_data_from_file(rate_stamp_file, 2, [rate_time, rate], ' ')

    frame_size_time = []
    frame_size = []
    read_data_from_file(frame_size_stamp_file, 2, [frame_size_time, frame_size], ' ')

    return rate_time, rate, frame_size_time, frame_size

def CalculateOverallDelay(rec_dir, res_dir):
    frame_size_file = os.path.join(rec_dir, 'frame_size_original_timestamp.log')
    end_stamp_file = os.path.join(rec_dir, 'end_stamp.log')
    start_stamp_file = os.path.join(rec_dir, 'start_stamp.log')
    overall_delay_file = os.path.join(res_dir, 'overall_delay.log')
    frame_encoded_time = []
    frame_end_index = []
    frame_end_time = []
    frame_start_index = []
    overall_delay = []

    read_data_from_file(frame_size_file, 2, [frame_encoded_time, []], ' ')
    read_data_from_file(end_stamp_file, 3, [[], frame_end_index, frame_end_time], ':')
    read_data_from_file(start_stamp_file, 3, [[], frame_start_index, []], ':')

    frame_encoded_time = [int(x) for x in frame_encoded_time]
    frame_end_index = [int(x) for x in frame_end_index]
    frame_end_time = [int(x) for x in frame_end_time]
    frame_start_index = [int(x) for x in frame_start_index]

    idx = 0
    for i in range(len(frame_end_index)):
        if i > 0 and frame_end_index[i] <= frame_end_index[i - 1]:
            continue
        for j in range(idx, len(frame_start_index)):
            if frame_end_index[i] == frame_start_index[j]:
                idx = j
                time_delay = int(frame_end_time[i]) - int(frame_encoded_time[j])
                overall_delay.append(time_delay)
                break

    write_data_to_file([range(len(overall_delay)), overall_delay], overall_delay_file)
    return overall_delay

def calc_delay_framesize_rate(recv_dir, res_dir):
    time_stamp_start = []
    time_stamp_end = []
    start_frame_idx = []
    end_frame_idx = []
    frame_delay = []
    frame_size = []

    recv_file = recv_dir + "recv.log"
    send_file = recv_dir + "send.log"
    start_time_stamp_file = recv_dir + "start_stamp.log"
    end_time_stamp_file = recv_dir + "end_stamp.log"
    frame_size_file = res_dir + "frame_size.log"
    delay_file = res_dir + "delay.log"
    rate_file = res_dir + "rate.log"

    os.system("rm -f " + start_time_stamp_file)
    os.system("rm -f " + end_time_stamp_file)
    os.system("rm -f " + frame_size_file)
    os.system("rm -f " + delay_file)
    os.system("rm -f " + rate_file)

    end_stamp_command = "grep \"Time Stamp\" " + recv_file + " | awk \'{print $4}\' > " + end_time_stamp_file
    start_stamp_command = "grep \"Time Stamp\" " + send_file + " | awk \'{print $4}\' > " + start_time_stamp_file
    os.system(start_stamp_command)
    os.system(end_stamp_command)

    read_data_from_file(start_time_stamp_file, 3, [[], start_frame_idx, time_stamp_start], ":")
    read_data_from_file(end_time_stamp_file, 3, [[], end_frame_idx, time_stamp_end], ":")

    idx = 0
    for i in range(len(time_stamp_end)):
        for j in range(idx, len(time_stamp_start)):
            if end_frame_idx[i] == start_frame_idx[j]:
                idx = j
                time_delay = time_stamp_end[i] - time_stamp_start[j]
                frame_delay.append(time_delay)
                break

    rate_time, rate, frame_size_time, frame_size = extract_rate_and_framesize(recv_dir)
    # frame size byte to kbps
    frame_size_bitrate = [(x * 8 * fps / 1000) for x in frame_size]
    start_time = min(rate_time[0], frame_size_time[0], time_stamp_end[0])
    rate_time = [x - start_time for x in rate_time]
    frame_size_time = [x - start_time for x in frame_size_time]
    time_stamp_end = [x - start_time for x in time_stamp_end]

    write_data_to_file([range(1, len(end_frame_idx) + 1), end_frame_idx, frame_size_time, frame_size, frame_size_bitrate], frame_size_file)
    write_data_to_file([rate_time, rate], rate_file)
    write_data_to_file([range(1, len(end_frame_idx) + 1), end_frame_idx, time_stamp_end, frame_delay], delay_file)

    overall_delay = CalculateOverallDelay(recv_dir, res_dir)

    return frame_delay, overall_delay

def calculate_send2receive_index(res_dir, send_frames_dir, receive_correspoding_send_index):
    send2receive_index_file = res_dir + '/send2receive_index.log'

    os.system("rm " + send2receive_index_file)

    send2receive_index_list = []

    f_send2receive_index_file = open(send2receive_index_file, 'w')

    send_start_index = receive_correspoding_send_index[0]
    send_frame_cnt = int(len(os.listdir(send_frames_dir)))
    receive_frame_cnt = len(receive_correspoding_send_index)
    print(send_start_index, receive_frame_cnt)

    receive2send_list_index = 0
    send_index = send_start_index
    last_receive_correspoding_send_index = -1
    while receive2send_list_index < receive_frame_cnt:
        current_receive_index = receive2send_list_index
        current_receive_correspoding_send_index = receive_correspoding_send_index[receive2send_list_index]
        # print(send_index, current_receive_correspoding_send_index, receive2send_list_index)
        while send_index < current_receive_correspoding_send_index:
            f_send2receive_index_file.write(str(send_index) + ',' + str(current_receive_index)+ '\n')
            # print(f'write {send_index},{current_receive_index}')
            send_index += 1
        if current_receive_correspoding_send_index < last_receive_correspoding_send_index:
            print(f"Reach the end of send file, {current_receive_correspoding_send_index} < {last_receive_correspoding_send_index}, {send_index}, {send_frame_cnt}")
            while send_index <= send_frame_cnt:
                f_send2receive_index_file.write(str(send_index) + ',' + str(current_receive_index)+ '\n')
                # print(f'write {send_index},{current_receive_index}')
                send_index += 1
            send_index = 1
            while send_index < current_receive_correspoding_send_index:
                f_send2receive_index_file.write(str(send_index) + ',' + str(current_receive_index)+ '\n')
                # print(f'write {send_index},{current_receive_index}')
                send_index += 1
        receive2send_list_index += 1
        last_receive_correspoding_send_index = current_receive_correspoding_send_index

    f_send2receive_index_file.close()

    return send2receive_index_list

def sync_frames(original_raw_frames_dir, rec_dir, res_dir, width, height):
    receive_raw_frames_dir = f'{rec_dir}raw_frames/'

    updated_send_file_prefix = f'{rec_dir}updated_send'
    updated_rec_file_prefix = f'{rec_dir}updated_recon'

    send2receive_index = []
    send_start_index = -1
    with open(f'{res_dir}send2receive_index.log', 'r') as f:
        for line in f.readlines():
            if send_start_index == -1:
                send_start_index = int(line.split(',')[0])
            send2receive_index.append([int(line.split(',')[0]), int(line.split(',')[1])])
    print(f"send_start_index: {send_start_index}")
    print(len(send2receive_index))

    send_frame_cnt = len(send2receive_index)
    f_output_yuv = open(f'{updated_send_file_prefix}.yuv', 'wb')
    for i in range(send_start_index + 10, len(send2receive_index)):
        send_img_path = original_raw_frames_dir + "frame" + str(send2receive_index[i][0]) + ".png"
        send_img = open(send_img_path, 'rb').read()
        # print(send_img_path)
        f_output_yuv.write(send_img)
    f_output_yuv.close()

    f_output_yuv = open(f'{updated_rec_file_prefix}.yuv', 'wb')
    for i in range(send_start_index + 10, len(send2receive_index)):
        rev_img_path = receive_raw_frames_dir + "frame" + str(send2receive_index[i][1]) + ".png"
        rev_img = open(rev_img_path, 'rb').read()
        # print(rev_img_path)
        f_output_yuv.write(rev_img)
    f_output_yuv.close()

    os.system(f'ffmpeg -s {width}x{height} -i {updated_rec_file_prefix}.yuv -c:v libx264 -preset superfast -qp 0 -y {updated_rec_file_prefix}.mp4')
    os.system(f'ffmpeg -s {width}x{height} -i {updated_send_file_prefix}.yuv -c:v libx264 -preset superfast -qp 0 -y {updated_send_file_prefix}.mp4')

def calculate_vmaf_score(rec_dir, send_filename, rec_filename):
    vmaf_command = f'docker run --rm -v {rec_dir}:/socket gfdavila/easyvmaf -r /socket/{send_filename}.mp4 -d /socket/{rec_filename}.mp4'
    os.system(vmaf_command)

def generate_head_result(data, ratio):
    data.sort()
    data_len = len(data)
    head_data = data[:int(data_len * ratio)]
    mean_head_data = np.mean(head_data)
    return mean_head_data

def read_json_file(rec_dir, vmaf_score_file, f_overall_vmaf_score):
    vmaf_score_json_file = f'{rec_dir}updated_recon_vmaf.json'
    f_vmaf_score = open(vmaf_score_file, 'w')
    vmaf_scores = []

    with open(vmaf_score_json_file, 'r') as f:
        datas = json.load(f)
        vmaf_mean = datas['pooled_metrics']['vmaf']['mean']
        vmaf_harmonic_mean = datas['pooled_metrics']['vmaf']['harmonic_mean']
        for frame in datas['frames']:
            vmaf_scores.append(float(frame['metrics']['vmaf']))
            f_vmaf_score.write(f"{frame['frameNum']},{frame['metrics']['vmaf']}\n")
        vmaf_scores = np.array(vmaf_scores)
        f_overall_vmaf_score.write(f"{vmaf_mean},{vmaf_harmonic_mean},{generate_head_result(vmaf_scores, 0.1)},{generate_head_result(vmaf_scores, 0.2)},{generate_head_result(vmaf_scores, 0.3)},{generate_head_result(vmaf_scores, 0.5)}\n")
    return vmaf_scores

def generate_vmaf_result(original_raw_frames_dir, rec_dir, res_dir, width, height, f_overall_vmaf_score):
    vmaf_score_file = f'{res_dir}vmaf_score.log'
    if os.path.exists(vmaf_score_file):
        os.remove(vmaf_score_file)

    sync_frames(original_raw_frames_dir, rec_dir, res_dir, width, height)
    calculate_vmaf_score(rec_dir, 'updated_send', 'updated_recon')

    vmaf_scores = read_json_file(rec_dir, vmaf_score_file, f_overall_vmaf_score)

    updated_send_file_prefix = f'{rec_dir}updated_send'
    updated_rec_file_prefix = f'{rec_dir}updated_recon'
    os.system(f'rm {updated_send_file_prefix}.mp4')
    os.system(f'rm {updated_rec_file_prefix}.mp4')
    os.system(f'rm {updated_send_file_prefix}.yuv')
    os.system(f'rm {updated_rec_file_prefix}.yuv')

    return vmaf_scores

def decode_recv_video(cfg):
    re_extract_images = True
    root_dir = os.path.dirname(os.getcwd()) + "/"
    recv_dir = f"{root_dir}result/{cfg.data}/rec/"
    res_dir = f"{root_dir}result/{cfg.data}/res/"

    os.system("mkdir -p " + res_dir)

    recv_video_path = recv_dir + "recon.yuv"
    recv_raw_frames_dir = recv_dir + "raw_frames/"

    send_raw_frames_dir = root_dir + "send/" + cfg.data + "/"
    receive_correspoding_file = res_dir + "receive_correspoding_index.log"

    if re_extract_images:
        os.system("rm -rf " + recv_raw_frames_dir)
        os.system("mkdir -p " + recv_raw_frames_dir)
        # Extract all frames from recevied yuv file
        ffmpeg_command = f"{ffmpeg_path} -r {fps} -s {cfg.width}x{cfg.height} \
          									-i {recv_video_path} {recv_raw_frames_dir}/frame%d.png -y"
        os.system(ffmpeg_command)
    received_frame_cnt = len(os.listdir(recv_raw_frames_dir))

    delay, overall_delay = calc_delay_framesize_rate(recv_dir, res_dir)
    drop_frames_index, receive_correspoding_send_index = scan_qrcode_fast(recv_raw_frames_dir, received_frame_cnt)
    print(f"Drop frames index: {drop_frames_index}")
    print(len(drop_frames_index))

    f_receive_correspoding = open(receive_correspoding_file, "w")
    for idx in range(len(receive_correspoding_send_index)):
        f_receive_correspoding.write(str(idx + 1) + "," + str(receive_correspoding_send_index[idx]) + "\n")
    f_receive_correspoding.close()

    calculate_send2receive_index(res_dir, send_raw_frames_dir, receive_correspoding_send_index)

    f_overall_vmaf_file = open('../overall_vmaf_score.log', 'a+')
    f_overall_vmaf_file.write(f"{cfg.data},{cfg.output_dir},")
    vmaf_scores = generate_vmaf_result(send_raw_frames_dir, recv_dir, res_dir, cfg.width, cfg.height, f_overall_vmaf_file)

    os.system("rm -rf " + recv_raw_frames_dir)

    return delay, drop_frames_index, vmaf_scores, overall_delay

## Send and receive video
def start_process(cmd, error_log_file=None):
    if error_log_file:
        with open(error_log_file, 'w') as f:
            return subprocess.Popen(cmd, shell=True, stdin=subprocess.PIPE, stdout=f, stderr=f, preexec_fn=os.setsid)
    else:
        return subprocess.Popen(cmd, shell=True, stdin=subprocess.PIPE, preexec_fn=os.setsid)

def kill_process(process):
    process.terminate() 
    process.wait()
    os.killpg(process.pid,signal.SIGKILL)

def generate_tail_result(data, ratio):
    data.sort()
    data_len = len(data)
    tail_data = data[int(data_len * (1 - ratio)):]
    mean_tail_data = np.mean(tail_data)
    return mean_tail_data

def send_and_recv_video(cfg):
    root_dir = os.path.dirname(os.path.dirname(os.getcwd())) + "/"

    client_bin = root_dir + "out/Default/peerconnection_localvideo"
    send_bin = root_dir + "out/Default/peerconnection_localvideo"
    
    server_ip = cfg.server_ip
    port = cfg.server_port

    recv_dir = f"{root_dir}my_experiment/result/{cfg.data}/rec/"
    recv_file = f"{recv_dir}recon.yuv"
    send_video_path = f"{root_dir}my_experiment/data/{cfg.data}_qrcode.yuv"

    server_command = f"{root_dir}out/Default/peerconnection_server --port {port} &"
    send_command = f"{send_bin} --file {send_video_path} --height {cfg.height}\
                                --width {cfg.width} --fps {fps}\
                                --server {server_ip} --port {port}\
                                --enable_mae {cfg.enable_mae} --disable_frame_drop {cfg.disable_frame_drop}"
    recv_command = f"{client_bin} --recon {recv_file} --server {server_ip} --port {port}"

    os.system("mkdir -p " + recv_dir)

    server_process = start_process(server_command)
    time.sleep(1)

    recv_process = start_process(recv_command, recv_dir + "recv.log")
    time.sleep(1)

    send_process = start_process(send_command, recv_dir + "send.log")
    send_process.wait()

    kill_process(recv_process)
    kill_process(server_process)

    delay, drop_frames_index, vmaf_scores, overall_delay = decode_recv_video(cfg)
    delay = np.array(delay)
    vmaf_scores = np.array(vmaf_scores)
    overall_delay = np.array(overall_delay)

    avg_delay = np.mean(delay)
    avg_vmaf = np.mean(vmaf_scores)
    avg_overall_delay = np.mean(overall_delay)
    
    print(f"----------- Current result for {cfg.data} is: ---------------")
    print(f"Average Delay: {avg_delay}, Tail 10% Delay: {generate_tail_result(delay.tolist(), 0.1)}")
    print(f"Average Overall Delay: {avg_overall_delay}, Tail 10% Overall Delay: {generate_tail_result(overall_delay.tolist(), 0.1)}")
    print("Average VMAF: ", avg_vmaf)
    print("Drop frames count: ", len(drop_frames_index))

## Show figure
def show_multi_plot_fig(files, x_indexes, y_indexes, labels, x_label, y_label, fig_file, start_index = 0, end_index = 10000):
    if len(files) != len(x_indexes) or len(files) != len(y_indexes):
        print("Please pass filename, x_indexes, y_indexes and lable for both file!")
        return

    colors = ['r', 'g', 'b', 'c', 'm', 'y']
    plt.figure(figsize = (16, 8))
    plt.xlabel(x_label, fontsize = 14)
    plt.ylabel(y_label, fontsize = 14)

    for idx in range(len(files)):
        file = files[idx]
        if not os.path.exists(file):
            continue

        data = []
        data_index = []

        with open(file, "r") as f:
            for lines in f.readlines():
                line = lines.split(",")
                value = float(line[y_indexes[idx]])
                if value > 0:
                    data_index.append(int(line[x_indexes[idx]]))
                    data.append(float(value))
        color = colors[idx]
        plt.plot(data_index[start_index:end_index], data[start_index:end_index], label = labels[idx], color = color)

    plt.legend()
    plt.grid()
    plt.savefig(fig_file, bbox_inches = 'tight', pad_inches = 0.1)

def show_experiment_fig(data_file, fig_file, x_index, y_index, label, x_label, start_index = 0, end_index = 10000):
    data = []
    data_index = []

    if not os.path.exists(data_file):
        return
    with open(data_file, "r") as f:
        for lines in f.readlines():
            line = lines.split(",")
            value = float(line[y_index])
            if value > 0:
                data_index.append(int(line[x_index]))
                data.append(float(value))

    plt.figure(figsize = (16, 8))
    plt.xlabel(x_label, fontsize = 14)
    plt.ylabel(label, fontsize = 14)
    plt.plot(data_index[start_index:end_index], data[start_index:end_index], label = label)
    plt.legend()
    plt.grid()
    plt.savefig(fig_file, bbox_inches = 'tight', pad_inches = 0.1)

def show_fig(cfg):
    fig_dir = f"../result/{cfg.data}/fig/"
    os.system("rm -rf " + fig_dir)
    os.system("mkdir -p " + fig_dir)

    res_dir = f"../result/{cfg.data}/res/"
    vmaf_log_file = res_dir + "vmaf_score.log"
    frame_size_file = res_dir + "frame_size.log"
    rate_file_with_frame_index = res_dir + "rate_with_frame_index.log"
    overall_delay_file = res_dir + "overall_delay.log"

    show_experiment_fig(vmaf_log_file, fig_dir + "/vmaf.png", 0, 1, "VMAF", "frame_index")
    show_experiment_fig(overall_delay_file, fig_dir + "/overall_delay.png", 0, 1, "Overall Delay(ms)", "Frame Index", 5)

    files = [rate_file_with_frame_index, frame_size_file]
    x_indexes = [0, 0]
    y_indexes = [2, 4]
    labels = ["Rate", "FrameSize"]
    show_multi_plot_fig(files, x_indexes, y_indexes, labels, "frame_index", "Rate(kbps)", fig_dir + "/rate_frame_size.png", 1)

def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--option", type=str)
    parser.add_argument("--data", type=str)
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    parser.add_argument("--server_ip", type=str)
    parser.add_argument("--server_port", type=str)
    parser.add_argument("--enable_mae", type=int, default=1)
    parser.add_argument("--disable_frame_drop", type=int, default=0)

    return parser.parse_args()

if __name__ == "__main__":
    cfg = parse_args()
    if cfg.option == "gen_send_video":
        overlay_qrcode_to_video(cfg)
    elif cfg.option == "send_and_recv":
        send_and_recv_video(cfg)
    elif cfg.option == "show_fig":
        show_fig(cfg)
    else:
        print("invalid option")