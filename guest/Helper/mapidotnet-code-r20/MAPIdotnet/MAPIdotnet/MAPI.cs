using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;
using System.Data;

namespace MAPIdotnet
{
    public class MAPI : IDisposable
    {
        private cemapi.IMAPISession session;
        private IMAPIMsgStore[] stores = null;
        private bool disposed;

        public MAPI()
        {
            this.session = cemapi.MAPILogon();
        }

        ~MAPI()
        {
            Dispose(false);
        }

        public void Dispose(bool disposing)
        {
            if(disposed == false && disposing)
            {
                foreach ( IMAPIMsgStore store in stores )
                {
                    store.Dispose();
                }
                this.session.Dispose();
                disposed = true;
            }
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        public IMAPIMsgStore[] MessageStores 
        { 
            get 
            {
                if (this.stores == null)
                {
                    cemapi.IMAPITable msgStores = this.session.GetMsgStoresTable();
                    msgStores.SetColumns(new cemapi.PropTags[] { cemapi.PropTags.PR_ENTRYID });

                    List<IMAPIMsgStore> stores = new List<IMAPIMsgStore>(4);
                    int len = 0;
                    do
                    {
                        cemapi.SRow[] rows = msgStores.QueryRows(cemapi.MaxQueryRowCount);
                        len = rows.Length;
                        if (!(len > 0))
                            break;
                        for (int i = 0; i < len; i++)
                            stores.Add(new MAPIMsgStore(this, this.session.OpenMsgStore(rows[i].propVals[0].AsBinary), this.session));
                    }
                    while (len >= cemapi.MaxQueryRowCount);
                    this.stores = stores.ToArray();

#if DEBUG
                    foreach(IMAPIMsgStore store in stores)
                    {
                        Console.WriteLine(store.EntryID.AsByteArray);
                    }
#endif
                }
                return this.stores;
            } 
        }

        public void DisplayMessage(IMAPIMessageID msg)
        {
            this.session.DisplayMessage(((MAPIMessageID)msg).AsByteArray);
        }

        public bool CompareEntryIDs(IEntryID entryID1, IEntryID entryID2)
        {
            return this.session.CompareEntryIDs(entryID1.AsByteArray, entryID2.AsByteArray);
        }
    }

    public enum TableSortOrder : uint
    {
        TABLE_SORT_ASCEND = 0x00000000,
        TABLE_SORT_DESCEND = 0x00000001,
        TABLE_SORT_COMBINE = 0x00000002,
    }

    public enum DeleteFolderResult : uint
    {
        Successful = 0,
        HasSubFolders = 1,
        HasMessages = 2
    }
}
