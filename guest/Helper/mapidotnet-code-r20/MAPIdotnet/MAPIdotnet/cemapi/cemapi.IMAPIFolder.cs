using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;

namespace MAPIdotnet
{
    internal static partial class cemapi
    {
        public interface IMAPIFolder : IMAPIContainer
        {
            void CopyMessages(byte[][] messageIDs, IMAPIFolder destFolder, bool msgMove);
            void DeleteMessages(byte[][] msgEntryIds);
            void DeleteMessage(byte[] msgEntryId);
            void EmptyFolder();
            IMAPIFolder CreateFolder(string folderName, bool openIfExist);
            DeleteFolderResult DeleteFolder(byte[] folderId, bool delSubFolders, bool delMsgs);
        }

        private class MAPIFolder : MAPIContainer, IMAPIFolder
        {
            [DllImport("MAPIlib.dll", EntryPoint = "IMAPIFolderDeleteMessages")]
            private static extern HRESULT pIMAPIFolderDeleteMessages(IntPtr folder, IntPtr lpMsgs);

            [DllImport("MAPIlib.dll", EntryPoint = "IMAPIFolderEmptyFolder")]
            private static extern HRESULT pIMAPIFolderEmptyFolder(IntPtr folder);

            [DllImport("MAPIlib.dll", EntryPoint = "IMAPIFolderCreateFolder")]
            private static extern HRESULT pIMAPIFolderCreateFolder(IntPtr folder, IntPtr lpszFolderName, uint flags, ref IntPtr newFolder);

            [DllImport("MAPIlib.dll", EntryPoint = "IMAPIFolderDeleteFolder")]
            private static extern HRESULT pIMAPIFolderDeleteFolder(IntPtr folder, uint cbEntryID, IntPtr lpEntryID, uint flags);

            [DllImport("MAPIlib.dll", EntryPoint = "IMAPIFolderCopyMessages")]
            private static extern HRESULT pIMAPIFolderCopyMessages(IntPtr folder, IntPtr lpMsgList, IntPtr lpDestFolder, uint flags);

            public MAPIFolder(IntPtr ptr) : base(ptr) { }

            public void DeleteMessages(byte[][] msgEntryIds)
            {
                IntPtr pMsgs = Marshal.AllocHGlobal(8);
                int numIds = msgEntryIds.Length;
                Marshal.WriteInt32(pMsgs, numIds);
                IntPtr pArray = Marshal.AllocHGlobal(8 * numIds);
                Marshal.WriteInt32(pMsgs, 4, (int)pArray);

                for (int i = 0; i < numIds; ++i)
                {
                    byte[] id = msgEntryIds[i];
                    int idLen = id.Length;
                    IntPtr bytes = Marshal.AllocHGlobal(idLen);
                    Marshal.WriteInt32(pArray, i * 8, idLen);
                    Marshal.WriteInt32(pArray, i * 8 + 4, (int)bytes);
                    Marshal.Copy(id, 0, bytes, idLen);
                }

                HRESULT hr = pIMAPIFolderDeleteMessages(this.ptr, pMsgs);
                if (hr != HRESULT.S_OK)
                    throw new Exception("Delete messages failed: " + hr.ToString());

                for (int i = 0; i < numIds; ++i)
                    Marshal.FreeHGlobal((IntPtr)Marshal.ReadInt32(pArray, 8 * i + 4));
                Marshal.FreeHGlobal(pArray);
                Marshal.FreeHGlobal(pMsgs);
            }

            public void DeleteMessage(byte[] msgEntryId)
            {
                int idLen = msgEntryId.Length;
                IntPtr pMsg = Marshal.AllocHGlobal(8);
                Marshal.WriteInt32(pMsg, 1);
                IntPtr pArray = Marshal.AllocHGlobal(8);
                Marshal.WriteInt32(pMsg, 4, (int)pArray);
                Marshal.WriteInt32(pArray, idLen);
                IntPtr entryBytes = Marshal.AllocHGlobal(idLen);
                Marshal.WriteInt32(pArray, 4, (int)entryBytes);
                Marshal.Copy(msgEntryId, 0, entryBytes, idLen);

                HRESULT hr = pIMAPIFolderDeleteMessages(this.ptr, pMsg);
                if (hr != HRESULT.S_OK)
                    throw new Exception("Delete messages failed: " + hr.ToString());

                Marshal.FreeHGlobal(entryBytes);
                Marshal.FreeHGlobal(pMsg);
                Marshal.FreeHGlobal(pArray);
            }

            public void EmptyFolder()
            {
                HRESULT hr = pIMAPIFolderEmptyFolder(this.ptr);
                if (hr != HRESULT.S_OK)
                    throw new Exception("Empty folder failed: " + hr.ToString());
            }

            public IMAPIFolder CreateFolder(string folderName, bool openIfExist)
            {
                IntPtr str = Marshal.StringToBSTR(folderName), newPtr = IntPtr.Zero;
                HRESULT hr = pIMAPIFolderCreateFolder(this.ptr, str, (uint)(CreateFolderFlags.MAPI_UNICODE | (openIfExist ? CreateFolderFlags.OPEN_IF_EXISTS : 0)), ref newPtr);
                if (hr != HRESULT.S_OK)
                    throw new Exception("pIMAPIFolderCreateFolder failed: " + hr.ToString());
                Marshal.FreeBSTR(str);
                return new MAPIFolder(newPtr);
            }

            public DeleteFolderResult DeleteFolder(byte[] folderId, bool delSubFolders, bool delMsgs)
            {
                IntPtr p = Marshal.AllocHGlobal(folderId.Length);
                Marshal.Copy(folderId, 0, p, folderId.Length);
                HRESULT hr = pIMAPIFolderDeleteFolder(this.ptr, (uint)folderId.Length, p, (uint)((delSubFolders ? DeleteFolderFlags.DEL_FOLDERS : 0) | (delMsgs ? DeleteFolderFlags.DEL_MESSAGES : 0)));
                DeleteFolderResult res = DeleteFolderResult.Successful;
                if (hr == HRESULT.MAPI_E_HAS_FOLDERS)
                    res = DeleteFolderResult.HasSubFolders;
                if (hr == HRESULT.MAPI_E_HAS_MESSAGES)
                    res |= DeleteFolderResult.HasMessages;
                else if (hr != HRESULT.S_OK && hr != HRESULT.MAPI_E_HAS_FOLDERS)
                    throw new Exception("pIMAPIFolderDeleteFolder failed: " + hr.ToString());

                Marshal.FreeHGlobal(p);
                return res;
            }

            public void CopyMessages(byte[][] messageIDs, IMAPIFolder destFolder, bool msgMove)
            {
                int numIds = messageIDs.Length;
                IntPtr sBinaryArray = Marshal.AllocHGlobal(4 * (numIds + 1));
                Marshal.WriteInt32(sBinaryArray, numIds);
                for (int i = 0; i < numIds; i++)
                {
                    byte[] id = messageIDs[i];
                    IntPtr idPtr = Marshal.AllocHGlobal(8);
                    IntPtr idVals = Marshal.AllocHGlobal(id.Length);
                    Marshal.Copy(id, 0, idVals, id.Length);
                    Marshal.WriteInt32(idPtr, id.Length);
                    Marshal.WriteIntPtr((IntPtr)((int)idPtr + 4), idVals);
                    Marshal.WriteIntPtr((IntPtr)((int)sBinaryArray + 4 * (i + 1)), idPtr);
                }
                HRESULT hr = pIMAPIFolderCopyMessages(this.Ptr, sBinaryArray, destFolder.Ptr, (uint)(msgMove ? 1 : 0));

                for (int i = 0; i < numIds; i++)
                {
                    IntPtr ptr = Marshal.ReadIntPtr((IntPtr)((int)sBinaryArray + 4*(i+1)));
                    Marshal.FreeHGlobal(Marshal.ReadIntPtr((IntPtr)((int)ptr + 4)));
                    Marshal.FreeHGlobal(ptr);
                }
                Marshal.FreeHGlobal(sBinaryArray);

                if (hr != HRESULT.S_OK)
                    throw new Exception("pIMAPIFolderCreateFolder failed: " + hr.ToString());
            }
        }


    }
}
