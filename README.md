RPihost
=======

RPi Host - for SLA printers

to start the fbi, i've now start it manualy from ssh connection

sudo ./fbi -vt 1 -noverbose -1 mijn/blank.png

in a second ssh connection i start the http python server by hand
python webs.py

now i can connect to the RPi via my webbrowser via port 8088
