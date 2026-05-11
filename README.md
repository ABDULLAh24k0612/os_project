# os_project
Autonomous Ride-Sharing Dispatch System (GUI Version)
=====================================================

This version uses GTK3 to provide a graphical interface for the simulation.

Prerequisites
-------------
You must install the GTK development libraries on your Ubuntu system.
1. Open your terminal.
2. Run the following command:
   sudo apt update && sudo apt install build-essential libgtk-3-dev

How to Compile
--------------
In your terminal, navigate to the folder containing gui_dispatch.c and run:
gcc dispatch_system.c -o dispatch_system `pkg-config --cflags --libs gtk+-3.0` -pthread

How to Run
----------
Execute the compiled program by running:
   ./dispatch

What to Expect
--------------
A graphical window will open natively on your desktop, displaying the active requests, surge pricing multiplier, and real-time status of the transit fleet. The simulation runs automatically and handles thread synchronization in the background.
