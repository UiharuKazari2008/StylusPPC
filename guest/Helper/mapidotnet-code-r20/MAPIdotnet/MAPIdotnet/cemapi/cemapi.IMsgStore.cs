using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;

namespace MAPIdotnet
{
    internal static partial class cemapi
    {
        #region IMsgStore
        public interface IMsgStore : IMAPIProp, IDisposable
        {
            IMAPISession Session { get; }

            IMAPIFolder OpenEntryAsFolder(byte[] entryId);
            IMessage OpenEntryAsMessage(byte[] entryId);

            byte[] GetReceiveFolder();

            IMAPIAdviseSink Advise(EEventMask mask);
        }

        internal enum ObjectType : uint
        {
            MAPI_STORE = 0x00000001,    /* Message Store */
            MAPI_ADDRBOOK = 0x00000002,    /* Address Book */
            MAPI_FOLDER = 0x00000003,    /* Folder */
            MAPI_ABCONT = 0x00000004,    /* Address Book Container */
            MAPI_MESSAGE = 0x00000005,    /* Message */
            MAPI_MAILUSER = 0x00000006,    /* Individual Recipient */
            MAPI_ATTACH = 0x00000007,    /* Attachment */
            MAPI_DISTLIST = 0x00000008,    /* Distribution List Recipient */
            MAPI_PROFSECT = 0x00000009,    /* Profile Section */
            MAPI_STATUS = 0x0000000A,    /* Status Object */
            MAPI_SESSION = 0x0000000B,    /* Session */
            MAPI_FORMINFO = 0x0000000C,    /* Form Information */
        };

        private class MsgStore : MAPIProp, IMsgStore
        {
            [DllImport("MAPIlib.dll", EntryPoint = "IMsgStoreOpenEntry")]
            private static extern HRESULT pIMsgStoreOpenEntry(IntPtr msgStore, ref SBinary entryId, ref ObjectType type, ref IntPtr entry);

            [DllImport("MAPIlib.dll", EntryPoint = "IMsgStoreGetReceiveFolder")]
            private static extern HRESULT pIMsgStoreGetReceiveFolder(IntPtr msgStore, out uint lpcbEntryID, out IntPtr lppEntryID);

            private IMAPISession session;

            public MsgStore(IntPtr pMsgStore, IMAPISession session) : base(pMsgStore) { this.session = session; }

            public IMAPISession Session { get { return this.session; } }

            public IMAPIFolder OpenEntryAsFolder(byte[] entryId)
            {
                IntPtr entryPtr = IntPtr.Zero;
                SBinary b = SBinaryCreate(entryId);
                ObjectType t = 0;
                HRESULT hr = pIMsgStoreOpenEntry(this.ptr, ref b, ref t, ref entryPtr);
                if (hr != HRESULT.S_OK)
                    throw new Exception("OpenEntryAsFolder failed: " + hr.ToString());
                SBinaryRelease(ref b);
                if (t == ObjectType.MAPI_FOLDER)
                {
                    IMAPIFolder folder = new MAPIFolder(entryPtr);
                    return folder;
                }
                else
                {
                    MAPIUnknown.ReleasePtr(entryPtr);
                    return null;
                }
            }

            public IMessage OpenEntryAsMessage(byte[] entryId)
            {
                IntPtr entryPtr = IntPtr.Zero;
                SBinary b = SBinaryCreate(entryId);
                ObjectType t = 0;
                HRESULT hr = pIMsgStoreOpenEntry(this.ptr, ref b, ref t, ref entryPtr);
                if (hr != HRESULT.S_OK)
                    throw new Exception("OpenEntryAsMessage failed: " + hr.ToString());
                SBinaryRelease(ref b);
                if (t == ObjectType.MAPI_MESSAGE)
                {
                    Message message = new Message(entryPtr);
                    return message;
                }
                else
                {
                    MAPIUnknown.ReleasePtr(entryPtr);
                    return null;
                }
            }

            public byte[] GetReceiveFolder()
            {
                uint cb = 0;
                IntPtr folderPtr = IntPtr.Zero;
                HRESULT hr = pIMsgStoreGetReceiveFolder(this.ptr, out cb, out folderPtr);
                if (hr != HRESULT.S_OK)
                    throw new Exception("pIMsgStoreGetReceiveFolder failed: " + hr.ToString());
                byte[] id = new byte[cb];
                Marshal.Copy(folderPtr, id, 0, (int)cb);
                MAPIFreeBuffer(folderPtr);
                return id;
            }

            public IMAPIAdviseSink Advise(EEventMask mask)
            {
                return new MAPIAdviseSink(this, mask, this.session);
            }

            
        }
        #endregion
    }
}
