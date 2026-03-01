SHELLISH - Assignment 1 COMP 304

Repo Link: https://github.com/iiftar22/COMP304-Project-1.git

I implemented the function process_command(struct command_t *command) as the main fucntion of my shell
It receives a parsed command structure and decides whether to execute it as a built-in command (handled inside the shell) or as an external command (executed via fork() + exec()) while also supporting redirection and pipelines.


Chatroom Command:


<img width="1352" height="637" alt="Chatroom 1" src="https://github.com/user-attachments/assets/8e991db6-bde5-4cc6-90d3-d6531258a269" />

<img width="1408" height="691" alt="Chatroom 2" src="https://github.com/user-attachments/assets/fe21200c-d4f0-40c3-9e12-7c578406fa95" />



Custom (Built-in) Command: Timer Command

This command lets the user set a timer that occupies the shell and showcases the time laps in the terminal. It accepts the folowing format 

    timer [positive int][h/m/s]
                                                            
h, m and s corresponds to hours, minutes and seconds. the user must use exactly one suffix and cannot choose anything other than h, m, s

The timer command uses colors, emojis and sounds to make the terminal usage more interesting. I gave a lot of attention for the timer to be readeable.

Here are some valid ways to use this command:


<img width="534" height="195" alt="timer command " src="https://github.com/user-attachments/assets/0bc4a778-aebb-4c4a-898d-9b8b7534431c" />

And some invalid ways:


<img width="537" height="192" alt="timer command invalid " src="https://github.com/user-attachments/assets/28d00af5-6e71-40e5-9305-5514ea7d24b0" />

Technical: I utilized a foreground process to restrict the user from typing any other commands while the 

Display Format: The countdown is dynamically updated on a single terminal line using the carriage return character (\r) to overwrite previous output. The time is displayed in HH:MM:SS format when at least o1 hour remains and automatically switches to MM:SS format once the remaining time drops below 1 hour.

Color Design: I used ANSI escape codes to enhance visual feedback. The countdown text appears in green during normal operation and switches to red during the final ten seconds.

Sound design: I generated an audible alert during the final five seconds by writing the ASCII Bell character (\a) directly to standard output using the write() system call. I chose to use write() instead of printf() to ensure immediate, low-level output without relying on buffered I/O mechanisms.
