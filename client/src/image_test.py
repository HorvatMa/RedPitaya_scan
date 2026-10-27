import threading
import queue
import numpy as np
from PIL import Image, ImageTk
import socket
import struct
import time
import tkinter as tk
from time import sleep
import scipy

RP_ADDRESS = "RP-F0A360.LOCAL"
PORT = 5000

def send_msg(msg, s):
    msg = struct.pack('>I', len(msg)) + msg

    print("Sending message!")
    s.sendall(msg)

    sleep(0.1)

    # msg = s.recv(1024)
    # print('Received: ' + msg.decode())

def send_msg_n_bytes(msg, s):
    msg = struct.pack('>I', len(msg)) + msg

    print("Sending message!")
    s.sendall(msg)

    msg = s.recv(5)
    N = int(msg.decode())

    print(N)
    print('Received: len = ' + msg.decode())

    return N

def transform_waveform(waveform):
    waveform *= 32767
    waveform = np.round(waveform)

    waveform = waveform.astype(np.int16)

    # fig, ax = plt.subplots()

    # ax.plot(waveform)

    # plt.show()
    # print(waveform)
    waveform = waveform.astype('>i2').tobytes()

    return waveform

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
        # print(f"Message length: {N}")
        
        # Receive the actual message (exactly N bytes)
        msg = recv_n_bytes(s, N)

        # print(f"Received!")
        
        return msg
    except (ValueError, socket.error) as e:
        print(f"Error in recv_msg: {e}")
        return b''


def tcp_data_receiver(sock, shared_queue):
    try:
        while True:
            byte_arr = recv_msg(sock)
            if not byte_arr:
                break

            record_dtype = np.dtype([('x', np.int16), ('y', np.int16), ('value', np.uint32)])
            record_size = record_dtype.itemsize

            # Trim incomplete trailing bytes if necessary
            if len(byte_arr) % record_size != 0:
                byte_arr = byte_arr[:-(len(byte_arr) % record_size)]

            if len(byte_arr) == 0:
                continue

            data = np.frombuffer(byte_arr, dtype=record_dtype)

            x_array = data['x'].astype(np.float32)
            y_array = data['y'].astype(np.float32)
            value_array = data['value'].astype(np.float32)

            shared_queue.put((x_array, y_array, value_array))

    except Exception as e:
        print(f"Error in TCP data receiver: {e}")
    finally:
        sock.close()
        print("TCP data receiver stopped.")


def image_generator(shared_queue, output_queue, x_min, x_max, y_min, y_max, resolution, interval_ms):
    """
    Thread function to generate or update an image based on received data (as arrays).

    Args:
        shared_queue (queue.Queue): A shared queue for receiving data.
        output_queue (queue.Queue): A shared queue for storing processed images.
        x_min, x_max (float): Limits for the x-dimension.
        y_min, y_max (float): Limits for the y-dimension.
        resolution (tuple): The resolution of the image (width, height).
        interval_ms (int): Time interval in milliseconds to create a fresh image.
    """
    img_width, img_height = resolution
    N_samples = 0
    start_time_speed = time.time()

    while True:
        # Initialize a new image and count array
        image = np.zeros((img_height, img_width), dtype=np.float32)
        count_array = np.zeros((img_height, img_width), dtype=np.int32)

        # Start timing for the interval
        start_time = time.time()
        if (time.time() - start_time_speed > 2):
            print("Actual speed: {:.2f} Mbit/s".format(N_samples*8*8 / 1e6))
            print("Queue len: {}".format(shared_queue.qsize()))
            N_samples = 0
            start_time_speed = time.time()

        while (time.time() - start_time) * 1000 < interval_ms:
            try:
                # Fetch data from the shared queue
                data = shared_queue.get(timeout=(interval_ms / 1000))
                shared_queue.task_done()

                x_array, y_array, value_array = data
                N_samples += len(x_array)

                # Define scaling factors for converting coordinates to pixel indices
                x_scale = (img_width - 1) / (x_max - x_min)
                y_scale = (img_height - 1) / (y_max - y_min)

                # Map x, y to image pixel coordinates
                x_indices = ((x_array - x_min) * x_scale).astype(int)
                y_indices = ((y_array - y_min) * y_scale).astype(int)

                # Mask to keep indices within bounds
                valid_mask = (
                    (x_indices >= 0) & (x_indices < img_width) &
                    (y_indices >= 0) & (y_indices < img_height)
                )

                # Apply the mask to the indices and values
                x_indices = x_indices[valid_mask]
                y_indices = y_indices[valid_mask]
                value_array = value_array[valid_mask]

                # Compute total accumulated values (image * count)
                total_accum = image * count_array

                # Sum new values per pixel
                sum_new_values = np.zeros_like(image)
                np.add.at(sum_new_values, (y_indices, x_indices), value_array)

                # Count new values per pixel
                new_counts = np.zeros_like(count_array)
                np.add.at(new_counts, (y_indices, x_indices), 1)

                # Update total accumulated values and counts
                total_accum += sum_new_values
                count_array += new_counts

                # Avoid division by zero
                nonzero_mask = count_array > 0
                image[nonzero_mask] = total_accum[nonzero_mask] / count_array[nonzero_mask]

            except queue.Empty:
                # If no data is received within the interval, proceed
                pass

        # Rescale the image to the range [0, 255]
        min_value = np.min(image)
        max_value = np.max(image)

        if max_value > min_value:  # Avoid division by zero
            rescaled_image = 255 * (image - min_value) / (max_value - min_value)
        else:
            rescaled_image = np.zeros_like(image)

        # Convert to uint8
        rescaled_image = rescaled_image.astype(np.uint8)

        # Add the processed image to the output queue
        output_queue.put(rescaled_image)


def gui_display(output_queue):
    """
    Function to create a Tkinter GUI and update the displayed image from the output queue.
    
    Args:
        output_queue (queue.Queue): The queue from which processed images are retrieved.
    """
    def update_image():
        try:
            # Get the latest image from the output queue
            # print(output_queue.qsize())
            rescaled_image = output_queue.get_nowait()
            img = ImageTk.PhotoImage(image=Image.fromarray(rescaled_image, 'L'))
            image_label.config(image=img)
            image_label.image = img  # Keep a reference to avoid garbage collection
        except queue.Empty:
            pass
        root.after(100, update_image)  # Schedule the next update

    root = tk.Tk()
    root.title("Real-Time Image Viewer")
    image_label = tk.Label(root)
    image_label.pack()
    root.after(100, update_image)
    root.mainloop()


if __name__ == "__main__":
    # Shared queues for communication between threads
    shared_queue = queue.Queue()  # For raw data
    output_queue = queue.Queue()  # For processed images

    # Parameters
    x_min, x_max = -11000, 11000
    y_min, y_max = -11000, 11000
    resolution = (200, 200)
    interval_ms = 666  # Create a fresh image every 1000 milliseconds

    # Start the image generation thread
    image_thread = threading.Thread(
        target=image_generator,
        args=(shared_queue, output_queue, x_min, x_max, y_min, y_max, resolution, interval_ms),
        daemon=True
    )
    image_thread.start()

    tcp_socket = socket.create_connection((RP_ADDRESS, PORT))

    waveform_len_1 = int(2*resolution[0])
    dec1 = int(125000000 * 0.95 * interval_ms / 5 / 1000 / waveform_len_1)
    waveform_len_2 = int(2*resolution[1])
    dec2 = int(dec1 * 1.9 / waveform_len_2)
    acq_dec = int(dec2 * 0.95)
    print(dec1, dec2, acq_dec)
    print("Speed: {:.2f} Mbit/s".format(125000000 / acq_dec * 64 / 1000000))

    msg = "set_lim {} {} {} {}".format(x_min, x_max, y_min, y_max)
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    msg = "set_res {} {}".format(resolution[0], resolution[1])
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    msg = "set_frame_time {}".format(interval_ms)
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    msg = "set_waveform 0 {}".format(waveform_len_1)
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    x = np.linspace(0, 1, waveform_len_1, dtype=np.float16)
    # waveform = np.sin(2*np.pi*x)*0.75
    waveform = scipy.signal.sawtooth(2*np.pi*x, 0.5)*0.3
    msg = transform_waveform(waveform)
    send_msg(msg, tcp_socket)

    msg = "set_gen_decimation 0 {}".format(dec1)
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    msg = "set_waveform 1 {}".format(waveform_len_2)
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    x = np.linspace(0, 1, waveform_len_2, dtype=np.float16)
    waveform = np.sin(2*np.pi*x)*0.3
    # waveform = scipy.signal.sawtooth(2*np.pi*x, 0.95)*0.5
    msg = transform_waveform(waveform)
    send_msg(msg, tcp_socket)

    msg = "set_gen_decimation 1 {}".format(dec2)
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    msg = "set_acq_decimation {}".format(acq_dec)
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    msg = "set_int_decimation 1"
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    # Start the TCP data receiver thread
    tcp_thread = threading.Thread(
        target=tcp_data_receiver,
        args=(tcp_socket, shared_queue),
        daemon=True
    )
    tcp_thread.start()

    msg = "start_gen"
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    msg = "start_acq"
    msg = str.encode(msg)
    send_msg(msg, tcp_socket)

    # Start the GUI
    gui_display(output_queue)
