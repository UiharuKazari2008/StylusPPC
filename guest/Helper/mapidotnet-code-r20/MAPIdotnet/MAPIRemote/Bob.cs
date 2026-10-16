using System;
using System.Xml;
using System.Xml.Serialization;
using System.Xml.Schema;
using System.Collections.Generic;
using System.Text;

namespace MAPIRemote
{



    [Serializable]
    public class Bob
    {
        //[SoapAttribute(Namespace = "http://www.cpandl.com")]
        private string b;
        //[SoapAttribute(DataType = "base64Binary")]
        //[SoapAttribute(DataType = "base64Binary")]
        public int a;
        public Bob() { }
        public Bob(int a, string b) { this.a = a; this.b = b; }
    }
}
