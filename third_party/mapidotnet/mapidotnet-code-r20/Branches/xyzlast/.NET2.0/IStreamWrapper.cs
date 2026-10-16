using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Text;
using OpenNETCF.Runtime.InteropServices.ComTypes;

namespace MAPIdotnet
{
    /* IStreamWrapper.cs
 * An OLE IStream wrapper implementation around native .NET Streams
 *
 * Based on work by Oliver Sturm (see remarks on class IStreamWrapper). I
 * basically just adapted this to work on .NET Compact Framework 2.0 and
 * added the requisite support constructs.
 *
 * Distributed with no license what-so-ever - feel free to do
 * whatever the hell you want with this :-)  I would appreciate it
 * if you dropped me a line to let me know if you've found this
 * useful though.
 *
 * Have fun!
 *
 * Tomer Gabel (http://www.tomergabel.com)
 * Monfort Software Engineering Ltd. (http://www.monfort.co.il)
 * E-mail me at tomer@tomergabel.com
 *
 */
    /// <summary>
    /// OLE IStream interface declaration
    /// </summary>
    /*[ComImport]
    [Guid("0000000c-0000-0000-C000-000000000046")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IStream
    {
        void Read([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] byte[] pv, int cb, IntPtr pcbRead);
        void Write([MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] byte[] pv, int cb, IntPtr pcbWritten);
        void Seek(long dlibMove, int dwOrigin, IntPtr plibNewPosition);
        void SetSize(long libNewSize);
        void CopyTo(IStream pstm, long cb, IntPtr pcbRead, IntPtr pcbWritten);
        void Commit(int grfCommitFlags);
        void Revert();
        void LockRegion(long libOffset, long cb, int dwLockType);
        void UnlockRegion(long libOffset, long cb, int dwLockType);
        void Stat(out STATSTG pstatstg, int grfStatFlag);
        void Clone(out IStream ppstm);
    }*/

    [ComImport]
    [Guid("0000000c-0000-0000-C000-000000000046")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IStreamChar
    {
        void Read([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] char[] pv, int cb, IntPtr pcbRead);
        void Write([MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] char[] pv, int cb, IntPtr pcbWritten);
        void Seek(long dlibMove, int dwOrigin, IntPtr plibNewPosition);
        void SetSize(long libNewSize);
        void CopyTo(IStream pstm, long cb, IntPtr pcbRead, IntPtr pcbWritten);
        void Commit(int grfCommitFlags);
        void Revert();
        void LockRegion(long libOffset, long cb, int dwLockType);
        void UnlockRegion(long libOffset, long cb, int dwLockType);
        void Stat(out STATSTG pstatstg, int grfStatFlag);
        void Clone(out IStream ppstm);
    }

    /// <summary>
    /// Win32 FILETIME structure declaration
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct FILETIME
    {
        public int dwLowDateTime;
        public int dwHighDateTime;
    }

    /// <summary>
    /// Win32 STATSTG structure declaration
    /// </summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct STATSTG
    {
        public string pwcsName;
        public int type;
        public long cbSize;
        public FILETIME mtime;
        public FILETIME ctime;
        public FILETIME atime;
        public int grfMode;
        public int grfLocksSupported;
        public Guid clsid;
        public int grfStateBits;
        public int reserved;
    }

    /// <summary>
    /// Wraps <see cref="System.IO.Stream"/> with a COM IStream implementation.
    /// </summary>
    /// <remarks>Based on work by <a href="http://www.sturmnet.org/blog/archives/2005/03/03/cds-csharp-extractor/">
    /// Oliver Sturm</a></remarks>
    /*[ComVisible(true)]
    [Guid("EB6EE2AB-5FE7-4467-AD4F-52656F8E1854")]
    [ClassInterface(ClassInterfaceType.None)]
    internal class IStreamWrapper : IStream
    {
        private Stream m_stream;

        /// <summary>
        /// Initializes a new instance of the <see cref="IStreamWrapper"/> class.
        /// </summary>
        /// <param name="stream">The stream to wrap.</param>
        public IStreamWrapper(Stream stream)
        {
            if (stream == null)
                throw new ArgumentNullException("stream");
            this.m_stream = stream;
        }

        #region IStream implementation
        void IStream.Clone(out IStream ppstm)
        {
            ppstm = null;
        }
        void IStream.Commit(int grfCommitFlags)
        {
        }
        void IStream.CopyTo(IStream pstm, long cb, System.IntPtr pcbRead, System.IntPtr pcbWritten)
        {
        }
        void IStream.LockRegion(long libOffset, long cb, int dwLockType)
        {
        }
        void IStream.Read(byte[] pv, int cb, System.IntPtr pcbRead)
        {
            Marshal.WriteInt64(pcbRead, m_stream.Read(pv, 0, cb));
        }
        void IStream.Revert()
        {
        }
        void IStream.Seek(long dlibMove, int dwOrigin, System.IntPtr plibNewPosition)
        {
            Marshal.WriteInt64(plibNewPosition, m_stream.Seek(dlibMove, (SeekOrigin)dwOrigin));
        }
        void IStream.SetSize(long libNewSize)
        {
        }
        void IStream.Stat(out STATSTG pstatstg, int grfStatFlag)
        {
            pstatstg = new STATSTG();
        }
        void IStream.UnlockRegion(long libOffset, long cb, int dwLockType)
        {
        }
        void IStream.Write(byte[] pv, int cb, System.IntPtr pcbWritten)
        {
        }
        #endregion
    }*/

    internal class StreamWrapper : Stream
    {
        private IStream s;
        private int size;
        private bool writeable;

        public StreamWrapper(IStream s, int size, bool writeable) { this.s = s; this.size = size; this.writeable = writeable; }

        public override int Read(byte[] buffer, int offset, int count)
        {
            IntPtr p = Marshal.AllocHGlobal(4);
            byte[] b = new byte[count];
            s.Read(b, count, p);
            int c = Marshal.ReadInt32(p);
            Marshal.FreeHGlobal(p);
            b.CopyTo(buffer, offset);
            return c;
        }

        public override long Seek(long offset, SeekOrigin origin)
        {
            IntPtr p = Marshal.AllocHGlobal(8);
            this.s.Seek(offset, (int)origin, p);
            long newLoc = Marshal.ReadInt64(p);
            Marshal.FreeHGlobal(p);
            return newLoc;
        }

        public override void Write(byte[] buffer, int offset, int count)
        {
            if (!this.writeable)
                return;
            IntPtr p = Marshal.AllocHGlobal(4);
            byte[] b = new byte[count];
            Array.Copy(buffer, offset, b, 0, count);
            this.s.Write(b, count, p);
            Marshal.FreeHGlobal(p);
        }

        public override bool CanRead
        {
            get { return true; }
        }
        public override bool CanWrite
        {
            get { return this.writeable; ; }
        }
        public override bool CanSeek
        {
            get { return true; }
        }

        public override long Length
        {
            get { return this.size; }
        }

        public override void Flush()
        {
            throw new NotImplementedException();
        }
        public override void SetLength(long value)
        {
            throw new NotImplementedException();
        }
        public override long Position
        {
            get
            {
                throw new NotImplementedException();
            }
            set
            {
                throw new NotImplementedException();
            }
        }
    }
}

