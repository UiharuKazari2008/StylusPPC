using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;
using OpenNETCF.Runtime.InteropServices.ComTypes;

namespace MAPIdotnet
{
	internal static partial class cemapi
	{
        public interface IMessage : IMAPIProp
        {
            IMAPITable GetRecipientTable();
            IMAPITable GetAttachmentTable();
            IAttach OpenAttach(int num);
        }

        private class Message : MAPIProp, IMessage
        {
            /// <summary>
            /// 
            /// </summary>
            /// <param name="msg"></param>
            /// <param name="recipTable"></param>
            /// <returns>MAPI_E_NO_RECIPIENTS if no recipients</returns>
            [DllImport("MAPIlib.dll", EntryPoint = "IMessageGetRecipientTable")]
            private static extern HRESULT pIMessageGetRecipientTable(IntPtr msg, ref IntPtr recipTable);

            [DllImport("MAPIlib.dll", EntryPoint = "IMessageGetAttachmentTable")]
            private static extern HRESULT pIMessageGetAttachmentTable(IntPtr msg, ref IntPtr recipTable);

            [DllImport("MAPIlib.dll", EntryPoint = "IMessageOpenAttach")]
            private static extern HRESULT pIMessageOpenAttach(IntPtr msg, uint num, ref IntPtr attachment);

            public Message(IntPtr ptr) : base(ptr) { }

            ~Message()
            { }

            public IMAPITable GetRecipientTable()
            {
                IntPtr tablePtr = IntPtr.Zero;
                HRESULT hr = pIMessageGetRecipientTable(this.ptr, ref tablePtr);
                if (hr == HRESULT.MAPI_E_NO_RECIPIENTS)
                    return null;
                if (hr != HRESULT.S_OK)
                    throw new Exception("pIMessageGetRecipientTable failed: " + hr.ToString());
                return new MAPITable(tablePtr);
            }

            public IMAPITable GetAttachmentTable()
            {
                IntPtr tablePtr = IntPtr.Zero;
                HRESULT hr = pIMessageGetAttachmentTable(this.ptr, ref tablePtr);
                if (hr != HRESULT.S_OK)
                    throw new Exception("pIMessageGetAttachmentTable failed: " + hr.ToString());
                return new MAPITable(tablePtr);
            }

            public IAttach OpenAttach(int num)
            {
                IntPtr ptr = IntPtr.Zero;
                HRESULT hr = pIMessageOpenAttach(this.ptr, (uint)num, ref ptr);
                if (hr != HRESULT.S_OK)
                    throw new Exception("pIMessageOpenAttach failed: " + hr.ToString());
                return new Attach(ptr);
            }
        }
	}
}
