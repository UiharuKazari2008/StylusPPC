using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;

namespace MAPIdotnet
{
    internal class MAPIMessage : MAPIProp, IMAPIMessage
    {
        private cemapi.IMessage msg;
        private IMAPIMessageID msgId;
        private string subject = null;
        private UInt64 deliveryTime = 0;
        private EMessageStatus status = 0;
        private bool statusDone = false, senderDone = false;
        private IMAPIContact sender = null;

        public MAPIMessage(cemapi.IMessage msg, IMAPIMessageID msgId) : base(msg, (msgId.ParentStore as MAPIMsgStore).Session) { this.msg = msg; this.msgId = msgId; }

        public override void Dispose()
        { }

        public IMAPIMessageID MessageID { get { return this.msgId; } }

        #region Message Properties
        public void PopulateProperties(EMessageProperties properties)
        {
            List<cemapi.PropTags> tags = new List<cemapi.PropTags>();
            if ((properties & EMessageProperties.Subject) != 0)
                tags.Add(cemapi.PropTags.PR_SUBJECT);
            if ((properties & EMessageProperties.DeliveryTime) != 0)
                tags.Add(cemapi.PropTags.PR_MESSAGE_DELIVERY_TIME);
            if ((properties & EMessageProperties.Status) != 0)
                tags.Add(cemapi.PropTags.PR_MSG_STATUS);
            if ((properties & EMessageProperties.Sender) != 0)
                tags.AddRange(new cemapi.PropTags[] { cemapi.PropTags.PR_SENDER_EMAIL_ADDRESS, cemapi.PropTags.PR_SENDER_NAME });

            cemapi.IPropValue[] props = this.msg.GetProps(tags.ToArray());

            int len = props.Length;
            for (int i = 0; i < props.Length; i++)
            {
                cemapi.IPropValue prop = props[i];
                switch (tags[i])
                {
                    case cemapi.PropTags.PR_SUBJECT:
                        if (prop.Type == typeof(string))
                            this.subject = prop.AsString;
                        break;
                    case cemapi.PropTags.PR_MESSAGE_DELIVERY_TIME:
                        if (prop.Type != null)
                            this.deliveryTime = prop.AsUInt64;
                        else
                            this.deliveryTime = 0xFFFFFFFFFFFFFFFF;
                        break;
                    case cemapi.PropTags.PR_MSG_STATUS:
                        this.status = (EMessageStatus)prop.AsInt32;
                        this.statusDone = true;
                        break;
                    case cemapi.PropTags.PR_SENDER_EMAIL_ADDRESS: // sender
                        if (prop.Tag != cemapi.PropTags.PR_ERROR)
                        {
                            this.sender = new MAPIContact(prop.AsString, props[i + 1].AsString, this.msgId.ParentFolder.Parent);
                            this.senderDone = true;
                        }
                        else
                        {
                            this.sender = null;
                            this.senderDone = false;
                        }
                        i++;
                        break;
                }
            }
        }

        public void InvalidateProperties(EMessageProperties properties)
        {
            if ((properties & EMessageProperties.Subject) != 0)
                this.subject = null;
            if ((properties & EMessageProperties.DeliveryTime) != 0)
                this.deliveryTime = 0;
            if ((properties & EMessageProperties.Status) != 0)
                this.statusDone = false;
            if ((properties & EMessageProperties.Sender) != 0)
                this.senderDone = false;
        }

        public string Body
        {
            get
            {
                IStreamChar s = (IStreamChar)this.msg.OpenProperty(cemapi.PropTags.PR_BODY, 0);
                if (s == null)
                    return "";
                IntPtr p = Marshal.AllocHGlobal(4);
                char[] b = new char[3];
                StringBuilder str = new StringBuilder();
                int c, len = b.Length * 2;
                do
                {
                    s.Read(b, len, p);
                    c = Marshal.ReadInt32(p);
                    str.Append(new string(b, 0, c / 2));
                }
                while (c >= len);
                Marshal.FreeHGlobal(p);
                return str.ToString();
            }
        }

        public string Subject
        {
            get
            {
                if (this.subject == null)
                {
                    cemapi.IPropValue val = this.msg.GetProps(new cemapi.PropTags[] { cemapi.PropTags.PR_SUBJECT })[0];
                    if (val.Tag != cemapi.PropTags.PR_SUBJECT)
                        this.subject = "";
                    else
                        this.subject = val.AsString;
                }
                return this.subject;
            }
        }

        public UInt64 SystemDeliveryTime
        {
            get
            {
                if (this.deliveryTime == 0)
                {
                    cemapi.IPropValue p = this.msg.GetProps(new cemapi.PropTags[] { cemapi.PropTags.PR_MESSAGE_DELIVERY_TIME })[0];
                    if (p.Type != null)
                        this.deliveryTime = p.AsUInt64;
                    else
                        this.deliveryTime = 0xFFFFFFFFFFFFFFFF;
                }

                return this.deliveryTime;
            }
        }

        public DateTime LocalDeliveryTime 
        { 
            get 
            {
                if (this.SystemDeliveryTime == 0 || this.SystemDeliveryTime == 0xFFFFFFFFFFFFFFFF)
                    return new DateTime(0);
                return (new DateTime((long)this.SystemDeliveryTime, DateTimeKind.Utc)).ToLocalTime().AddYears(1600); 
            } 
        }

        public EMessageStatus Status
        {
            get
            {
                if (this.statusDone == false)
                {
                    this.status = (EMessageStatus)this.msg.GetProps(new cemapi.PropTags[] { cemapi.PropTags.PR_MSG_STATUS })[0].AsInt32;
                    this.statusDone = true;
                }
                return this.status;
            }
        }

        public EMessageFlags Flags
        {
            get
            {
                return (EMessageFlags)this.msg.GetProps(new cemapi.PropTags[] { cemapi.PropTags.PR_MESSAGE_FLAGS })[0].AsInt32;
            }
            set
            {
                this.msg.SetProps(new cemapi.PropTags[] { cemapi.PropTags.PR_MESSAGE_FLAGS }, new object[] { ((uint)value) });
            }
        }

        public IMAPIContact Sender
        {
            get
            {
                if (!this.senderDone)
                {
                    cemapi.IPropValue[] props = this.msg.GetProps(new cemapi.PropTags[] { cemapi.PropTags.PR_SENDER_EMAIL_ADDRESS, cemapi.PropTags.PR_SENDER_NAME });
                    if (props[0].Tag != cemapi.PropTags.PR_ERROR)
                    {
                        this.sender = new MAPIContact(props[0].AsString, props[1].AsString, this.msgId.ParentFolder.Parent);
                        this.senderDone = true;
                    }
                    else
                    {
                        this.sender = null;
                        this.senderDone = false;
                    }
                }
                return this.sender;
            }
        }

        public IMAPIContact[] Recipients
        {
            get
            {
                cemapi.IMAPITable recipients = this.msg.GetRecipientTable();
                if (recipients == null)
                    return new IMAPIContact[0];
                // Fixed columns:
                // [0] - PR_ROWID
                // [1] - PR_DISPLAY_NAME
                // [2] - PR_EMAIL_ADDRESS
                // [3] - PR_ADDRTYPE
                // [4] - PR_RECIPIENT_TYPE

                List<IMAPIContact> contacts = new List<IMAPIContact>();
                int count;
                do
                {
                    cemapi.SRow[] rows = recipients.QueryRows(cemapi.MaxQueryRowCount);
                    count = rows.Length;
                    IMAPIContact[] c = new IMAPIContact[count];
                    for (int i = 0; i < count; i++)
                    {
                        c[i] = new MAPIContact(rows[i].propVals[2].AsString,
                            rows[i].propVals[1].AsString,
                            this.msgId.ParentFolder.Parent);
                    }
                    contacts.AddRange(c);
                }
                while (count >= cemapi.MaxQueryRowCount);

                return contacts.ToArray();
            }
        }

        public IMAPIAttachment[] Attachments
        {
            get
            {
                cemapi.IMAPITable attachTbl = this.msg.GetAttachmentTable();
                attachTbl.SetColumns(new cemapi.PropTags[] { cemapi.PropTags.PR_ATTACH_NUM, 
                    cemapi.PropTags.PR_ATTACH_SIZE, 
                    cemapi.PropTags.PR_ATTACH_FILENAME});

                List<IMAPIAttachment> attachments = null;
                int count;
                do
                {
                    cemapi.SRow[] rows = attachTbl.QueryRows(cemapi.MaxQueryRowCount);
                    count = rows.Length;
                    if (count == 0)
                        return new IMAPIAttachment[0];
                    else if (attachments == null)
                        attachments = new List<IMAPIAttachment>(count);
                    IMAPIAttachment[] a = new IMAPIAttachment[count];
                    for (int i = 0; i < count; i++)
                    {
                        cemapi.IPropValue[] props = rows[i].propVals;
                        a[i] = new MAPIAttachment(props[2].Tag == cemapi.PropTags.PR_ATTACH_FILENAME ? props[2].AsString : "",
                            props[0].AsInt32,
                            props[1].AsInt32,
                            this);
                    }
                    attachments.AddRange(a);
                }
                while (count >= cemapi.MaxQueryRowCount);

                return attachments.ToArray();
            }
        }

        public cemapi.IAttach OpenAttach(int attachNum)
        {
            return this.msg.OpenAttach(attachNum);
        }
        #endregion

        /*#region Operators
        public static bool operator ==(MAPIMessage a, MAPIMessage b)
        {
            if (a == null || b == null)
                return false;
            return a.EntryID == b.EntryID;
        }

        public static bool operator !=(MAPIMessage a, MAPIMessage b)
        {
            return !(a == b);
        }

        public override bool Equals(object obj)
        {
            return ((MAPIMessage)obj == this);
        }

        public override int GetHashCode()
        {
            return this.EntryID.GetHashCode();
        }
        #endregion*/
    }

    internal class MAPIMessageID : EntryID, IMAPIMessageID
    {
        private IMAPIFolderID parentFolder;
        private IMAPIMsgStore parentStore;
        public MAPIMessageID(byte[] id, IMAPIFolderID parentFolder, IMAPIMsgStore parentStore) : base(id, (parentStore as MAPIMsgStore).Session) { this.parentFolder = parentFolder; this.parentStore = parentStore; }
        private MAPIMessageID(IEntryID id, IMAPIFolderID parentFolder, IMAPIMsgStore parentStore) : base(id, (parentStore as MAPIMsgStore).Session) { this.parentFolder = parentFolder; this.parentStore = parentStore; }
        public static MAPIMessageID BuildFromID(IEntryID id, IMAPIFolderID parentFolder, IMAPIMsgStore parentStore)
        {
            if (id == null)
                return null;
            return new MAPIMessageID(id, parentFolder, parentStore);
        }

        public IMAPIFolderID ParentFolder { get { return this.parentFolder; } }
        public IMAPIMsgStore ParentStore { get { return this.parentStore; } }

        public IMAPIMessage OpenMessage()
        {
            return this.parentStore.OpenMessage(this);
        }

        public bool Equals(IMAPIMessageID folder)
        {
            return Equals((IEntryID)folder);
        }
    }
}
