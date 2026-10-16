using System;
using System.Xml;
using System.Xml.Serialization;
using System.Collections.Generic;
using System.Text;
using MAPIdotnet;
using System.IO;

namespace MAPIRemote
{

    public class Class1
    {

        

        private static String UTF8ByteArrayToString(Byte[] characters)
        {

            UTF8Encoding encoding = new UTF8Encoding();
            String constructedString = encoding.GetString(characters, 0, characters.Length);
            return (constructedString);
        }

        private static Byte[] StringToUTF8ByteArray(String pXmlString)
        {
            UTF8Encoding encoding = new UTF8Encoding();
            Byte[] byteArray = encoding.GetBytes(pXmlString);
            return byteArray;
        } 


        public static void Serialize(IMAPIMessage msg)
        {

            XmlTypeMapping myMapping = (new SoapReflectionImporter().ImportTypeMapping(typeof(Bob)));
            XmlSerializer mySerializer = new XmlSerializer(myMapping);
            //XmlSerializer mySerializer = new XmlSerializer(typeof(Bob));

            //Bob b = new Bob(34, "sdf");
            Bob b = new Bob(45, "yo");
            //b.a = 45;
            //b.b = "yo";

            MemoryStream memoryStream = new MemoryStream();

            mySerializer.Serialize(memoryStream, b);
            String XmlizedString = null;
            XmlizedString = UTF8ByteArrayToString(memoryStream.ToArray());


            memoryStream = new MemoryStream();
            XmlSerializer xs = new XmlSerializer(typeof(IMAPIMessage));
            xs.Serialize(memoryStream, msg);
            //XmlizedString = UTF8ByteArrayToString(memoryStream.ToArray());


            XmlSerializer xs1 = new XmlSerializer(typeof(IMAPIMessage), new Type[] { typeof(IMAPIMessageID), 
                typeof(DateTime),
                typeof(EMessageStatus),
                typeof(IMAPIContact),
                typeof(IMAPIAttachment) });
            memoryStream.Seek(0, SeekOrigin.Begin);
            Object o = xs1.Deserialize(memoryStream);

            

            /*Bob b = new Bob(34, "sdf");
            //XmlSerializer s = new XmlSerializer(typeof(IMAPIMessage));
            XmlSerializer s = new XmlSerializer(typeof(Bob));

            MemoryStream stream = new MemoryStream(100000);
            TextWriter writer = new StreamWriter(stream);

            s.Serialize(stream, b);
            stream.Seek(0, SeekOrigin.Begin);
            s = new XmlSerializer(typeof(Bob));
            TextReader tr = new StreamReader(stream);
            Bob b2 = (Bob)s.Deserialize(tr);

            s = new XmlSerializer(typeof(IMAPIMessage));
            s.Serialize(writer, msg);*/
            //StreamReader reader = new StreamReader(stream);
            
        }


    }
}
