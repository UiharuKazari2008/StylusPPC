using System;
using System.Collections.Generic;
using System.Text;
using System.IO;
using System.Runtime.InteropServices;
using OpenNETCF.Runtime.InteropServices.ComTypes;

namespace MAPIdotnet
{
    internal class MAPIAttachment : IMAPIAttachment
    {
        private string fileName;
        private int attachNum;
        private int size;
        private MAPIMessage msg;

        public MAPIAttachment(string fileName, int attachNum, int size, MAPIMessage msg)
        { this.fileName = fileName; this.attachNum = attachNum; this.size = size; this.msg = msg; }

        public string FileName { get { return this.fileName; } }

        public int Size { get { return this.size; } }

        public IMAPIMessage Message { get { return this.msg; } }

        public Stream OpenAttachment(bool writeAccess)
        {
            cemapi.IAttach attach = this.msg.OpenAttach(this.attachNum);
            IStream s = attach.OpenProperty(cemapi.PropTags.PR_ATTACH_DATA_BIN, writeAccess ? cemapi.OpenPropertyFlags.Default : 0);
            return new StreamWrapper(s, this.size, writeAccess);
        }

        public IStream OpenAttachmentNative(bool writeAccess)
        {
            cemapi.IAttach attach = this.msg.OpenAttach(this.attachNum);
            return attach.OpenProperty(cemapi.PropTags.PR_ATTACH_DATA_BIN, writeAccess ? cemapi.OpenPropertyFlags.Default : 0);
        }
    }
}
