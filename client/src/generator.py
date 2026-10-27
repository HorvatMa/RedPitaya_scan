import tkinter as tk
from tkinter import ttk
import numpy as np
import scipy.signal
import socket
from time import sleep
import struct

RP_ADDRESS = "RP-F0A360.LOCAL"
PORT = 5000

def transform_waveform_2(waveform, offset):
    waveform *= 32767
    waveform = np.round(waveform)

    waveform = waveform.astype(np.int16) + np.round(offset*32767).astype(np.int16)

    waveform = waveform.astype('>i2').tobytes()

    return waveform

def send_msg(msg, s):
    msg = struct.pack('>I', len(msg)) + msg

    print("Sending message!")
    s.sendall(msg)

    sleep(0.1)

# Main application
class WaveformApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Waveform Controller")

        # Default values
        self.amp1 = tk.DoubleVar(value=0.130 / 2)
        self.amp2 = tk.DoubleVar(value=0.179 / 2)
        self.offset1 = tk.DoubleVar(value=-0.915)
        self.offset2 = tk.DoubleVar(value=3.8)

        # Create sliders
        self.create_slider("Amplitude 1", self.amp1, 0.0, 1.0)
        self.create_slider("Amplitude 2", self.amp2, 0.0, 1.0)
        self.create_slider("Offset 1", self.offset1, -5.0, 5.0)
        self.create_slider("Offset 2", self.offset2, -5.0, 5.0)

        # Execute button
        execute_button = ttk.Button(self.root, text="Execute", command=self.execute)
        execute_button.pack(pady=10)

        # Start and Stop buttons
        start_button = ttk.Button(self.root, text="Start Generator", command=self.start_gen)
        start_button.pack(pady=5)

        stop_button = ttk.Button(self.root, text="Stop Generator", command=self.stop_gen)
        stop_button.pack(pady=5)

        self.tcp_socket = socket.create_connection((RP_ADDRESS, PORT))

        freq1 = 3
        freq2 = 1000

        self.waveform_len_1 = 3000
        dec1 = int(125e6 / self.waveform_len_1 / freq1)
        self.waveform_len_2 = 3000
        dec2 = int(125e6 / self.waveform_len_2 / freq2)

        msg = "set_gen_decimation 0 {}".format(dec1)
        msg = str.encode(msg)
        send_msg(msg, self.tcp_socket)

        msg = "set_gen_decimation 1 {}".format(dec2)
        msg = str.encode(msg)
        send_msg(msg, self.tcp_socket)

    def create_slider(self, label, variable, min_val, max_val):
        frame = ttk.Frame(self.root)
        frame.pack(fill="x", pady=5)

        lbl = ttk.Label(frame, text=label, width=15)
        lbl.pack(side="left")

        slider = ttk.Scale(frame, from_=min_val, to=max_val, orient="horizontal", variable=variable)
        slider.pack(side="left", fill="x", expand=True, padx=5)

        value_label = ttk.Label(frame, textvariable=variable, width=10)
        value_label.pack(side="right")

    def execute(self):
        # Parameters
        factor = 10.75
        amp1 = self.amp1.get()
        offset1 = self.offset1.get()
        amp2 = self.amp2.get()
        offset2 = self.offset2.get()

        # Waveform 1
        msg = "set_waveform 0 {}".format(self.waveform_len_1)
        msg = str.encode(msg)
        send_msg(msg, self.tcp_socket)

        x = np.linspace(0, 1, self.waveform_len_1, dtype=np.float16)
        waveform = scipy.signal.sawtooth(2 * np.pi * x, 0.975) * amp1
        waveform /= factor
        msg = transform_waveform_2(waveform, offset1 / factor)
        send_msg(msg, self.tcp_socket)

        # Waveform 2
        msg = "set_waveform 1 {}".format(self.waveform_len_2)
        msg = str.encode(msg)
        send_msg(msg, self.tcp_socket)

        x = np.linspace(0, 1, self.waveform_len_2, dtype=np.float16)
        waveform = np.sin(2 * np.pi * x) * amp2
        waveform /= factor
        msg = transform_waveform_2(waveform, offset2 / factor)
        send_msg(msg, self.tcp_socket)

    def start_gen(self):
        msg = "start_gen"
        msg = str.encode(msg)
        send_msg(msg, self.tcp_socket)

    def stop_gen(self):
        msg = "stop_gen"
        msg = str.encode(msg)
        send_msg(msg, self.tcp_socket)

if __name__ == "__main__":
    root = tk.Tk()
    app = WaveformApp(root)
    root.mainloop()
