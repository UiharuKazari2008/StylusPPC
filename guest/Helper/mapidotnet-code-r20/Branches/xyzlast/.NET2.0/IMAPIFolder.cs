using System;
using System.Collections.Generic;
using System.Text;

namespace MAPIdotnet
{
    public interface IMAPIFolder : IMAPIProp, IEquatable<IMAPIFolderID>
    {
        IMAPIMsgStore ParentStore { get; }
        IMAPIFolderID ParentFolder { get; }

        int NumSubFolders { get; }
        int NumSubItems { get; }

        IMAPIFolderID[] GetSubFolders(int maxNum);
        void SeekMessages(int position);
        IMAPIMessage[] GetNextMessages(int maxNum);
        int MessagesCursor();

        void SortMessagesByDeliveryTime(TableSortOrder order);
        void SortMessagesBySenderName(TableSortOrder order);
        void SortMessagesBySize(TableSortOrder order);

        IMAPIFolderID FolderID { get; }

        void DeleteMessages(IMAPIMessageID[] msgs);
        void DeleteMessage(IMAPIMessageID id);
        void EmptyFolder();
        void CopyMessages(IMAPIMessageID[] messages, IMAPIFolder destFolder, bool moveMsgs);

        IMAPIFolder CreateFolder(string name, bool openIfExist);
        DeleteFolderResult DeleteFolder(IMAPIFolderID folderId, bool delSubFolders, bool delMsgs);
    }

}
