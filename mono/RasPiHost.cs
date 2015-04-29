using System.Diagnostics;
using System.Net.Sockets;
using System.Threading;
using RasPiHost;
class Program
{
    static void Main()
    {
    //
    // Use Process.Start here.
    //
        BuildManager bManager = new BuildManager();
        bManager.PreparePrint("/home/pi/triado.zip");
        Thread.Sleep(4); 
        bManager.StartPrint();

        while(true)
        {
            Thread.Sleep(4); 
        }
        
       
    }
}
