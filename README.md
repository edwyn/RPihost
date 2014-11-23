RPihost
=======

RPi Host - for SLA printers

to start the fbi, i've now start it manualy from ssh connection

sudo ./fbi -vt 1 -noverbose -1 mijn/blank.png

the blanco png file you can find when you download the tar.gz file

in a second ssh connection i start the http python server by hand
python webs.py

now i can connect to the RPi via my webbrowser via port 8088
i'll also add my temp python scripts to activate the fbi with some slices

you can activate you RPi via the web browser by putting this in the url

http://192.168.0.249:8088/go

http://192.168.0.249:8088/upload
