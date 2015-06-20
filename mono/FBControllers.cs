using System.Diagnostics;
using System.Net.Sockets;
using System.Threading;

namespace RasPiHost
{
    class FBController
    {
        private int m_port = 0;
        private System.Net.Sockets.TcpClient m_clientSocket = null;
        private string m_server_address = null;

        public FBController()
        {
            m_port = 8888;
            m_clientSocket = new System.Net.Sockets.TcpClient();
            m_server_address = "127.0.0.1";
        }

        public void Disconnect()
        {
            if(m_clientSocket.Connected)
            {
                m_clientSocket.Close();
            }
        }

        public void Connect()
        {
            if(m_clientSocket != null)
            {
                m_clientSocket.Connect(m_server_address, m_port);
            }
        }

        public void Send(string command)
        {
            NetworkStream serverStream = m_clientSocket.GetStream();
            byte[] outStream = System.Text.Encoding.ASCII.GetBytes(command + "$");
            serverStream.Write(outStream, 0, outStream.Length);
            serverStream.Flush();
        }
    }
}
