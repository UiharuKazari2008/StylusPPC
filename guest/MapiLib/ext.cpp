// Additions to MAPIdotnet's MAPIlib (third_party/mapidotnet, MAPIlib/MAPIlib.cpp) in its own style:
// the calls the text-message bridge needs that MAPIlib does not wrap. Built into MAPIlib.dll by
// tools/make-mapilib.py.
#include "MAPIlib.h"

extern "C" {

AFX_EXT_CLASS HRESULT IMAPIFolderCreateMessage(IMAPIFolder *folder, IMessage **msg)
{
	return folder->CreateMessage(NULL, 0, msg);
}

AFX_EXT_CLASS HRESULT IMAPIPropSaveChanges(IMAPIProp *prop, ULONG flags)
{
	return prop->SaveChanges(flags);
}

AFX_EXT_CLASS HRESULT IMessageCreateAttach(IMessage *msg, ULONG *num, IAttach **attachment)
{
	return msg->CreateAttach(NULL, 0, num, attachment);
}

AFX_EXT_CLASS HRESULT IMessageModifyRecipients(IMessage *msg, ULONG flags, LPADRLIST mods)
{
	return msg->ModifyRecipients(flags, mods);
}

}  // extern "C"
