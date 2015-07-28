using System;
using System.Collections;
using System.Collections.Specialized;
using System.IO;
using System.Net;
using System.Reflection;
using System.Text;
using System.Threading;
 
class Program
{
    static void Main()
    {
       HttpListener listener = new HttpListener();
       listener.Prefixes.Add("http://*:8088/");
       listener.Start();
       Console.WriteLine("Listening...");
       for(;;)
       {
          HttpListenerContext ctx = listener.GetContext();
          new Thread(new Worker(ctx).ProcessRequest).Start();
       }
    }
 
    class Worker
    {
      private HttpListenerContext context;
 
       public Worker(HttpListenerContext context)
       {
          this.context = context;
       }
 
       public void ProcessRequest()
       {
          string msg = context.Request.HttpMethod + " " + context.Request.Url;
          Console.WriteLine(msg);
          switch (context.Request.HttpMethod)
          {
              case "GET":
                  do_get();
                  break;
              case "POST":
                  SaveFile(context.Request.ContentEncoding, GetBoundary(context.Request.ContentType), context.Request.InputStream);
                  Console.WriteLine(context.Request.Url);
                  context.Response.StatusCode = 200;
                  context.Response.ContentType = "text/html";
                  using (StreamWriter writer = new StreamWriter(context.Response.OutputStream, Encoding.UTF8))
                  writer.WriteLine("File Uploaded");
                  break;
          }
       }

       private void do_get()
       {
           StringBuilder sb = new StringBuilder();
           using (System.IO.StreamReader Reader = new System.IO.StreamReader("/home/pi/mono/src.html"))
          {
              string fileContent = Reader.ReadToEnd();
              sb.Append(fileContent);
          }
          byte[] b = Encoding.UTF8.GetBytes(sb.ToString());
          context.Response.ContentLength64 = b.Length;
          context.Response.OutputStream.Write(b, 0, b.Length);
          context.Response.OutputStream.Close();
       }

       private static String GetBoundary(String ctype)
       {
          return "--" + ctype.Split(';')[1].Split('=')[1];
       }

       private static String GetFileName(String ctype)
       {
          return ctype.Split(';')[2].Split('=')[1];
       }

       private static void SaveFile(Encoding enc, String boundary, Stream input)
       {
           Byte[] boundaryBytes = enc.GetBytes(boundary);
           Int32 boundaryLen = boundaryBytes.Length;

           //using (FileStream output = new FileStream("/home/pi/mono/data", FileMode.Create, FileAccess.Write))
           //{
               Byte[] buffer = new Byte[1024];
               Byte[] buffer2 = new Byte[1024];
               string Disposition;
               Int32 len = input.Read(buffer, 0, 1024);
               Int32 startPos = -1;
               Int32 preStartPos = 0;
               string filename = "";

               // Find start boundary
               while (true)
               {
                   if (len == 0)
                   {
                       throw new Exception("Start Boundaray Not Found");
                   }

                   startPos = IndexOf(buffer, len, boundaryBytes);
                   if (startPos >= 0)
                   {
                       break;
                   }
                   else
                   {
                       Array.Copy(buffer, len - boundaryLen, buffer, 0, boundaryLen);
                       len = input.Read(buffer, boundaryLen, 1024 - boundaryLen);
                   }
               }

               // Skip four lines (Boundary, Content-Disposition, Content-Type, and a blank)
               for (Int32 i = 0; i < 4; i++)
               {
                   while (true)
                   {
                       if (len == 0)
                       {
                           throw new Exception("Preamble not Found.");
                       }
                       
                       preStartPos = startPos;
                       startPos = Array.IndexOf(buffer, enc.GetBytes("\n")[0], startPos);
                       Array.Copy(buffer, preStartPos, buffer2, 0, startPos-preStartPos);
                       Disposition = System.Text.Encoding.UTF8.GetString(buffer2);
                       if(Disposition.Contains("Disposition"))
                       {
                           //Console.WriteLine("found dispostion " + Disposition);
                           filename = GetFileName(Disposition);
                           filename = filename.Split('"')[1];
                           Console.WriteLine("Found filename " + filename);
                       }

                       if (startPos >= 0)
                       {
                           startPos++;
                           break;
                       }
                       else
                       {
                           len = input.Read(buffer, 0, 1024);
                       }
                   }
               }

               Array.Copy(buffer, startPos, buffer, 0, len - startPos);
               len = len - startPos;

           using (FileStream output = new FileStream("/home/pi/mono/" + filename, FileMode.Create, FileAccess.Write))
           {

               while (true)
               {
                   Int32 endPos = IndexOf(buffer, len, boundaryBytes);
                   if (endPos >= 0)
                   {
                       if (endPos > 0) output.Write(buffer, 0, endPos);
                       break;
                   }
                   else if (len <= boundaryLen)
                   {
                       throw new Exception("End Boundaray Not Found");
                   }
                   else
                   {
                       output.Write(buffer, 0, len - boundaryLen);
                       Array.Copy(buffer, len - boundaryLen, buffer, 0, boundaryLen);
                       len = input.Read(buffer, boundaryLen, 1024 - boundaryLen) + boundaryLen;
                   }
               }
           }
       }

    private static Int32 IndexOf(Byte[] buffer, Int32 len, Byte[] boundaryBytes)
    {
        for (Int32 i = 0; i <= len - boundaryBytes.Length; i++)
        {
            Boolean match = true;
            for (Int32 j = 0; j < boundaryBytes.Length && match; j++)
            {
                match = buffer[i + j] == boundaryBytes[j];
            }

            if (match)
            {
                return i;
            }
        }

        return -1;
    } 
       private void DumpRequest(HttpListenerRequest request, StringBuilder sb)
       {
          DumpObject(request, sb);
       }
 
       private void DumpObject(object o, StringBuilder sb)
       {
          DumpObject(o, sb, true);
       }
 
       private void DumpObject(object o, StringBuilder sb, bool ulli)
       {
          if (ulli)
             sb.Append("<ul>");
 
          if (o is string || o is int || o is long || o is double)
          {
             if(ulli)
                sb.Append("<li>");
 
             sb.Append(o.ToString());
 
             if(ulli)
                sb.Append("</li>");
          }
          else
          {
             Type t = o.GetType();
             foreach (PropertyInfo p in t.GetProperties(BindingFlags.Public | BindingFlags.Instance))
             {
                sb.Append("<li><b>" + p.Name + ":</b> ");
                object val = null;
 
                try
                {
                   val = p.GetValue(o, null);
                }
                catch {}
 
                if (val is string || val is int || val is long || val is double)
                   sb.Append(val);
                else
 
                if (val != null)
                {
                   Array arr = val as Array;
                   if (arr == null)
                   {
                      NameValueCollection nv = val as NameValueCollection;
                      if (nv == null)
                      {
                         IEnumerable ie = val as IEnumerable;
                         if (ie == null)
                            sb.Append(val.ToString());
                         else
                            foreach (object oo in ie)
                               DumpObject(oo, sb);
                      }
                      else
                      {
                         sb.Append("<ul>");
                         foreach (string key in nv.AllKeys)
                         {
                            sb.AppendFormat("<li>{0} = ", key);
                            DumpObject(nv[key],sb,false);
                            sb.Append("</li>");
                         }
                         sb.Append("</ul>");
                      }
                   }
                   else
                      foreach (object oo in arr)
                         DumpObject(oo, sb);
                }
                else
                {
                   sb.Append("<i>null</i>");
                }
                sb.Append("</li>");
             }
          }
          if (ulli)
             sb.Append("</ul>");
       }
    }
}
