using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Timers;
using System.Threading;
using System.IO;
using System.Diagnostics;


namespace RasPiHost
{
    public class BuildManager
    {
        private const int STATE_START = 0;
        private const int STATE_DO_NEXT_COMMAND = 1;
        private const int STATE_WAITING_FOR_DELAY = 2;
        private const int STATE_CANCELLED = 3;
        private const int STATE_IDLE = 4;
        private const int STATE_DONE = 5;
        private const int STATE_WAIT_DISPLAY = 6; // waiting for the display to finish

        int              m_state = STATE_IDLE; // the state machine variable
        private Thread   m_runthread; // a thread to run all this..
        private bool     m_running; // a var to control thread life
        private DateTime m_printstarttime;

        private FBController m_fbcontroller = null;
        private bool m_printing = false;
        private bool m_paused = false;
        private DateTime m_buildstarttime;

        /*file management */
        private string   m_OutputFolder = null;
        private string   m_delaysfile = null;
        private string   m_gcode = null;
        private string[] gcodelines = null;
        int              m_gcodeline = 0; // which line of GCode are we currently on.


        public BuildManager()
        {
            m_fbcontroller = new FBController();
        }

        public void Setup()
        {



        }

        public void SetCode(string gcode)
        {

        }

        public void m_PrepareDelays()
        {
            bool readDelay = false;
            m_delaysfile = m_OutputFolder + "/delays.txt";
            
            string sPattern = "<Slice>";
            string bPattern = "Blank";
            string createText = "";

            
            var lines = File.ReadAllLines(m_gcode);
            foreach (var line in lines)
            {
                if(readDelay)
                {
                    readDelay = false;
                    string[] words = line.Split(' ');
                   // Console.WriteLine("readDelay " + words[0] + " second " + words[1] + "\n");
                    createText += words[1] + Environment.NewLine;
                }
                if (line.ToLower().Contains(sPattern.ToLower()))
                {
                    if(!line.ToLower().Contains(bPattern.ToLower()))
                    {
                        readDelay = true;
                    }
                }

            }
            //close filestream
            File.WriteAllText(m_delaysfile, createText);
            lines = null;
            //fileStream.Close();

        }

        //public void m_preparePNGfiles(var PNGlist)
      //  {
//
       // }

        public void m_getvarfromline(int[] line)
        {

        }

        /* unzip hack */
        private static void m_unzipFile(string zipFile, string outputFolder)
        {
            if (Type.GetType("Mono.Runtime") == null) return; //It is not mono === not linux!
            string arg = String.Format("-qq {0} -d {1}", zipFile, outputFolder);
            //string arg = String.Format("-c \"import serial; serial.Serial('{0}', {1})\"", portName, baudRate);
            var proc = new Process
            {
                EnableRaisingEvents = false,
                StartInfo = { FileName = @"/usr/bin/unzip", Arguments = arg }
            };
            proc.Start();
            proc.WaitForExit();
        }

        /// <summary>
        /// PreparePrint function unpacks the received CW file and parses the gcode 
        /// </summary>
        /// <param name="zipFilePath"></param>
        public void PreparePrint(string zipFilePath)
        {
            m_OutputFolder = "/home/pi/python/raspihost/output";

            // clean the output folder
            System.IO.DirectoryInfo downloadedMessageInfo = new DirectoryInfo(m_OutputFolder);
            foreach (FileInfo file in downloadedMessageInfo.GetFiles())
            {
                file.Delete();
            }
            foreach (DirectoryInfo dir in downloadedMessageInfo.GetDirectories())
            {
                dir.Delete(true);
            }

            if(zipFilePath != null)
            {
                m_unzipFile(zipFilePath, m_OutputFolder);
            } else
            {
                return;
            }

            var files = Directory.EnumerateFiles(m_OutputFolder, "*.*", SearchOption.AllDirectories)
            .Where(s => s.EndsWith(".gcode"));


            //Path to gcode file
            m_gcode = (string)files.First();

            //fill string arry with content gcode file
            gcodelines = File.ReadAllLines(m_gcode);

            //prepare png files and delays
            m_PrepareDelays();

            //get all Png files in the directory and sort them
            var PNGfiles = Directory.EnumerateFiles(m_OutputFolder, "*.*", SearchOption.AllDirectories)
            .Where(s => s.EndsWith(".png"));

            var sort = from s in PNGfiles
                       orderby s
                       select s;
            File.WriteAllLines(m_OutputFolder + "/png.txt", sort);

            m_fbcontroller.Connect();
            m_fbcontroller.Send("b");





        }

        public void StartPrint()
        {
            m_printing = true;
            m_buildstarttime = new DateTime();
            m_buildstarttime = DateTime.Now;
            //estimatedbuildtime = EstimateBuildTime(gcode);
            //StartBuildTimer();
            m_state = STATE_START; // set the state machine as started
            m_runthread = new Thread(new ThreadStart(BuildThread));
            m_running = true;
            m_runthread.Start();
        }

        private static int getvarfromline(String line)
        {
            try
            {
                int val = 0;
                line = line.Replace(';', ' '); // remove comments
                line = line.Replace(')', ' ');
                String[] lines = line.Split('>');
                if (lines[1].ToLower().Contains("blank"))
                {
                    val = -1; // blank screen
                }
                else if (lines[1].Contains("Special_"))
                {
                    val = -3; // special image
                }
              //  else if (lines[1].Contains("outline"))
              //  { //;<slice> outline XXX
                    // 
             //       val = SLICE_OUTLINE;
                    //still need to pull the number
              //      String[] lns2 = lines[1].Trim().Split(' ');
              //      outlinelayer = int.Parse(lns2[1].Trim()); // second should be variable
              //  }
                else
                {
                    String[] lns2 = lines[1].Trim().Split(' ');
                    val = int.Parse(lns2[0].Trim()); // first should be variable
                }

                return val;
            }
            catch (Exception ex)
            {
              //  DebugLogger.Instance().LogError(line);
               // DebugLogger.Instance().LogError(ex);
                return 0;
            }
        }

        int GetTimerValue()
        {
            return Environment.TickCount;
        }

        /*
         This is the thread that controls the build process
         * it needs to read the lines of gcode, one by one
         * send them to the printer interface,
         * wait for the printer to respond,
         * and also wait for the layer interval timer
         */
        void BuildThread()
        {
            int nextlayertime = 0;
            while(m_running)
            {
                try
                {
                    Thread.Sleep(0); //  sleep for 1 ms max
                    switch (m_state)
                    {
                        case BuildManager.STATE_START:
                            m_state = BuildManager.STATE_DO_NEXT_COMMAND; // go to the first layer
                            m_gcodeline = 0; // set the start line
                            m_printstarttime = new DateTime();
                            break;
                        case BuildManager.STATE_WAITING_FOR_DELAY: // general delay statement
                            if (GetTimerValue() >= nextlayertime)
                            {
                                m_state = BuildManager.STATE_DO_NEXT_COMMAND; // move onto next layer
                            }
                            else
                            {
                                Thread.Sleep(1);
                            }
                            break;
                        case BuildManager.STATE_IDLE:
                            // do nothing
                            Thread.Sleep(1);
                            break;
                        case BuildManager.STATE_WAIT_DISPLAY: 
                            // we're waiting on the display to tell us we're done
                            // this is used in the LaserSLA plugin, the normal DLP mode uses just a simple <Delay> command
                            //do nothing
                            break;
                        case BuildManager.STATE_DO_NEXT_COMMAND:
                            if (m_gcodeline >= gcodelines.Length)
                            {
                                m_state = BuildManager.STATE_DONE;
                                continue;
                            }
                            string line = "";
                            line = gcodelines[m_gcodeline++];
                            line = line.Trim();
                            if (line.Length > 0) // if the line is not blank
                            {
                                // send  the line, whether or not it's a comment - this is for a reason....
                                // should check to see if the firmware is ready for another line

                                //UVDLPApp.Instance().m_deviceinterface.SendCommandToDevice(line + "\r\n");
                                if (line.ToLower().Contains("<delay> "))// get the delay
                                {
                                    nextlayertime = GetTimerValue() + getvarfromline(line);
                                    //DebugLogger.Instance().LogInfo("Next Layer time: " + nextlayertime.ToString());
                                    m_state = BuildManager.STATE_WAITING_FOR_DELAY;
                                    continue;
                                }
                                else if (line.ToLower().Contains("<slice> "))//get the slice number
                                {
                                    if (line.ToLower().Contains("blank"))
                                    {
                                        //send blank screen
                                        Console.WriteLine("Show Blank screen \n");
                                        m_fbcontroller.Send("c");
                                    }
                                    else
                                    {
                                        //send next picture and red next line
                                        Console.WriteLine("Show Slice screen \n");
                                        m_fbcontroller.Send("n");
                                    }
                                }
                            }

                            break;
                        case BuildManager.STATE_DONE:
                            m_running = false;
                            m_state = BuildManager.STATE_IDLE;
                            break;

                       
                    }
                }
                catch (Exception ex)
                {

                }
            }
        }


    }


}

