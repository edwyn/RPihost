from threading import Thread
from fbcontroller import FBController
import zipfile
import os, shutil
from glob import glob
import operator
import time


class BuildManager():
    def __init__(self):
        self.fbcontroller               = None
        self.m_gcode                    = None
        self.m_gcodeline                = None
        self.m_gcodelines               = None
        self.m_printstarttime           = None
        self.delaysfile                 = None
        self.pngfiles                   = None
        self.outputfolder               = None
        self.m_running                  = None
        self.m_state                    = None
        self.STATE_START                = 0
        self.STATE_DO_NEXT_COMMAND      = 1
        self.STATE_WAITING_FOR_DELAY    = 2
        self.STATE_CANCELLED            = 3
        self.STATE_IDLE                 = 4
        self.STATE_DONE                 = 5
        self.STATE_WAIT_DISPLAY         = 6; #waiting for the display to finish
        

    def Setup(self, gcode=None, port=None):
        """setup and initialize conrollers
        """
        self.fbcontroller = FBController()
        #creat connection to motion
        #initialize and setup connection
        if port is not None:
            self.port = port
        if self.port is not None:
            self.fbconroller.connect(self.port)
        if gcode is not None:
            self.m_gcode = gcode
            

    def SetGcode(self, gcode):
        self.m_gcode = gcode

    def _prepareDelays(self):
        print("Start _prepareDelays")
        readDelay = False
        self.delaysfile = self.outputfolder + str('/delays.txt')
        f = open(self.delaysfile, 'w')
        print("opening gcode file to read " + str(self.m_gcode))
        with open(self.m_gcode) as gcodeobject:
            for line in gcodeobject:
                if readDelay:
                    readDelay = False
                    result = line.split()
                    f.write(result[1] + '\n')
                if '<Slice>' in line:
                    if "Blank" not in line:
                        readDelay = True
        f.close()

    def _preparePNGfiles(self, PNGlist):
        self.pngfiles = self.outputfolder + str('/png.txt')
        f = open(self.pngfiles, 'w')
        for item in PNGlist:
            f.write("%s\n" % item)
        f.close()

    def _getvarfromline(self, line):
        result = line.split()
        return int(result[1])
        

        

    def PreparePrint(self, zipfilePath=None):
        """unpack cw.zip file and inform fbcontroller to prepare
        """
        folder = '/home/pi/python/raspihost/output'
        self.outputfolder = folder
        #first clean the folder
        for the_file in os.listdir(folder):
            file_path = os.path.join(folder, the_file)
            try:
                if os.path.isfile(file_path):
                    os.unlink(file_path)
                elif os.path.isdir(file_path): shutil.rmtree(file_path)
            except Exception, e:
                print e
        #unzip cw.zip in cleaned folder
        if zipfilePath is not None:
           with zipfile.ZipFile(str(zipfilePath), 'r') as z:
               z.extractall(folder)
        #find the gcode file in folder
        result = [y for x in os.walk(folder) for y in glob(os.path.join(x[0], '*.gcode'))]
        print(result)
        self.m_gcode = str(result[0])
        print(self.m_gcode)
        if self.m_gcode is not None:
            gfh = open(self.m_gcode, 'r')
            self.m_gcodelines = gfh.readlines()
            print('number of lines in gcode file ' + str(len(self.m_gcodelines)))
            gfh.close()
        #prepare png files and delays
        self._prepareDelays()
        result = [y for x in os.walk(folder) for y in glob(os.path.join(x[0], '*.png'))]
        result.sort()
        self._preparePNGfiles(result)
        #prepare framebufferserver
        #send prepare command to framebuffer
        
    def StartPrint(self):
        self.m_running = True
        self.m_state   = self.STATE_START
        print('StartPrint m_state = ' + str(self.m_state))
        Thread(target=self.BuildThread).start()      

    def BuildThread(self):
        """main building thread
        """
        print('BuildThread started')
        nextlayertime = 0
        ts = time.time()
        #starttime = datetime.datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S')
        while(self.m_running):
            if self.m_state == self.STATE_START:
                self.m_state          = self.STATE_DO_NEXT_COMMAND #go to fhe first layer
                self.m_gcodeline      = 0
                self.m_printstarttime = time.time()
                #break
            if self.m_state == self.STATE_WAITING_FOR_DELAY:
                if int(round(time.time() * 1000)) >= nextlayertime:
                    self.m_state = self.STATE_DO_NEXT_COMMAND
                #sleep 1 ms
                time.sleep(0.005)
                #break
            if self.m_state == self.STATE_IDLE:
                continue
            if self.m_state == self.STATE_WAIT_DISPLAY:
                continue
            if self.m_state == self.STATE_DO_NEXT_COMMAND:
                #print('STATE_DO_NEXT_COMMAND ' + str(len(self.m_gcodelines)))
                if self.m_gcodeline >= len(self.m_gcodelines):
                    self.m_state = self.STATE_DONE
                    continue
                    
                line = None
                #go through the gcode, line by line
                line = self.m_gcodelines[self.m_gcodeline]
                self.m_gcodeline = self.m_gcodeline + 1
                line = line.strip()
                if len(line) > 0:
                    #send the line, whether or not it is a comment - thi is for a reason...
                    #should check to see if the firmware is ready for another line
                    #printcore.send(self.printcore.send(line)
                    if "<delay> " in line.lower():
                        nextlayertime = int(round(time.time() * 1000)) + self._getvarfromline(line)
                        self.m_state = self.STATE_WAITING_FOR_DELAY
                        continue
                    if "<slice> " in line.lower():
                        if "Blank" not in line:
                            #send next picture and read next line
                            #framebufferservice.send('n')
                            print('slice with normal picture')
                        else:
                            #send blank screen 
                            #framebufferservce.send('c')
                            print('slice with blank picture')
                            
                #break
            if self.m_state == self.STATE_DONE:
                self.m_running = False
                self.m_state   = self.STATE_IDLE
                #break
        


