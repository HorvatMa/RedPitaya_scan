import threading
import multiprocessing
import queue
import numpy as np
from PIL import Image, ImageTk
import socket
import struct
import time
import tkinter as tk
from time import sleep
from scipy import signal
from numba import njit

RP_ADDRESS = "RP-F0A360.LOCAL"
PORT = 5000

def send_msg(msg, s):
    msg = struct.pack('>I', len(msg)) + msg
    s.sendall(msg)
    sleep(0.1)

def recv_n_bytes(s, n):
    """
    Receives exactly n bytes from the socket.
    """
    chunks = []
    bytes_recd = 0
    while bytes_recd < n:
        chunk = s.recv(n - bytes_recd)
        if not chunk:
            raise socket.error("Socket connection broken")
        chunks.append(chunk)
        bytes_recd += len(chunk)
    return b''.join(chunks)

def recv_msg(s):
    """
    Receives a message with a 7-byte length header followed by the message body.
    Ensures exactly N bytes are received in the second part.
    """
    try:
        # Receive the 7-byte header
        header = recv_n_bytes(s, 10)  # Reliable read for 7 bytes
        # print(header)
        
        # Decode the header to get the length
        N = int(header.decode())
        print(f"Message length: {N}")
        # print
        
        # Receive the actual message (exactly N bytes)
        msg = recv_n_bytes(s, N)

        # print(f"Received!")
        
        return msg
    except (ValueError, socket.error) as e:
        print(f"Error in recv_msg: {e}")
        return b''

def transform_waveform(waveform):
    waveform = (waveform * 32767).astype(np.int16)
    return waveform.astype('>i2').tobytes()

@njit
def process_image(image, count_array, x_indices, y_indices, value_array):
    total_accum = image * count_array
    sum_new_values = np.zeros_like(image)
    new_counts = np.zeros_like(count_array)

    # Manually apply the operation
    for i in range(len(x_indices)):
        sum_new_values[y_indices[i], x_indices[i]] += value_array[i]
        new_counts[y_indices[i], x_indices[i]] += 1

    total_accum += sum_new_values
    count_array += new_counts

    nonzero_mask = count_array > 0
    image[nonzero_mask] = total_accum[nonzero_mask] / count_array[nonzero_mask]

    return image, count_array

def tcp_data_receiver(sock, shared_queue):
    try:
        while True:
            byte_arr = recv_msg(sock)
            if not byte_arr:
                break
            record_dtype = np.dtype([('x', np.int16), ('y', np.int16), ('value', np.uint32)])
            record_size = record_dtype.itemsize
            byte_arr = byte_arr[:-(len(byte_arr) % record_size)]
            if not byte_arr:
                continue
            data = np.frombuffer(byte_arr, dtype=record_dtype)
            x_array, y_array, value_array = data['x'], data['y'], data['value']
            shared_queue.put((x_array.astype(np.float32), y_array.astype(np.float32), value_array.astype(np.float32)))
    except Exception as e:
        print(f"TCP Receiver Error: {e}")
    finally:
        sock.close()

def image_generator(shared_queue, output_queue, x_min, x_max, y_min, y_max, resolution, interval_ms):
    img_width, img_height = resolution
    while True:
        image = np.zeros((img_height, img_width), dtype=np.float32)
        count_array = np.zeros((img_height, img_width), dtype=np.int32)
        start_time = time.time()
        while (time.time() - start_time) * 1000 < interval_ms:
            try:
                data = shared_queue.get(timeout=(interval_ms / 1000))
                x_array, y_array, value_array = data
                x_scale = (img_width - 1) / (x_max - x_min)
                y_scale = (img_height - 1) / (y_max - y_min)
                x_indices = ((x_array - x_min) * x_scale).astype(int)
                y_indices = ((y_array - y_min) * y_scale).astype(int)
                valid_mask = (0 <= x_indices) & (x_indices < img_width) & (0 <= y_indices) & (y_indices < img_height)
                image, count_array = process_image(
                    image, count_array,
                    x_indices[valid_mask], y_indices[valid_mask],
                    value_array[valid_mask]
                )
            except queue.Empty:
                pass
        min_val, max_val = np.min(image), np.max(image)
        rescaled_image = np.zeros_like(image) if max_val <= min_val else 255 * (image - min_val) / (max_val - min_val)
        output_queue.put(rescaled_image.astype(np.uint8))

def gui_display(output_queue):
    def update_image():
        try:
            img_data = output_queue.get_nowait()
            img = ImageTk.PhotoImage(image=Image.fromarray(img_data, 'L'))
            image_label.config(image=img)
            image_label.image = img
        except queue.Empty:
            pass
        root.after(100, update_image)

    root = tk.Tk()
    root.title("Real-Time Image Viewer")
    image_label = tk.Label(root)
    image_label.pack()
    root.after(100, update_image)
    root.mainloop()

if __name__ == "__main__":
    manager = multiprocessing.Manager()
    shared_queue = manager.Queue()
    output_queue = manager.Queue()
    x_min, x_max, y_min, y_max = -11000, 11000, -11000, 11000
    resolution = (200, 200)
    interval_ms = 333

    tcp_socket = socket.create_connection((RP_ADDRESS, PORT))
    tcp_thread = threading.Thread(target=tcp_data_receiver, args=(tcp_socket, shared_queue), daemon=True)
    tcp_thread.start()

    image_process = multiprocessing.Process(
        target=image_generator,
        args=(shared_queue, output_queue, x_min, x_max, y_min, y_max, resolution, interval_ms)
    )
    image_process.start()

    # Device configuration commands
    commands = [
        f"set_lim {x_min} {x_max} {y_min} {y_max}",
        f"set_res {resolution[0]} {resolution[1]}",
        f"set_frame_time {interval_ms}",
        f"set_gen_decimation 0 {int(125000000 / resolution[0])}",
        f"set_gen_decimation 1 {int(125000000 / resolution[1])}",
        "set_acq_decimation 10",
        "set_int_decimation 1"
    ]
    for cmd in commands:
        send_msg(cmd.encode(), tcp_socket)

    # Waveform generation and transmission
    waveform_len_1, waveform_len_2 = 2 * resolution[0], 2 * resolution[1]
    waveforms = [
        (waveform_len_1, signal.sawtooth(2 * np.pi * np.linspace(0, 1, waveform_len_1), 0.5) * 0.3),
        (waveform_len_2, np.sin(2 * np.pi * np.linspace(0, 1, waveform_len_2)) * 0.3)
    ]

    for i, (length, waveform) in enumerate(waveforms):
        send_msg(f"set_waveform {i} {length}".encode(), tcp_socket)
        send_msg(transform_waveform(waveform), tcp_socket)

    # Start generation and acquisition
    start_commands = ["start_gen", "start_acq"]
    for cmd in start_commands:
        send_msg(cmd.encode(), tcp_socket)

    # Start GUI display
    gui_display(output_queue)
