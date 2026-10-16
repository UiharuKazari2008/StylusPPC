using System;
using System.Collections.Generic;
using System.Text;

namespace MAPIdotnet
{
    internal static partial class cemapi
    {
        public interface IAttach : IMAPIProp
        {
            // Doesn't have any methods of its own
        }

        private class Attach : MAPIProp, IAttach
        {
            public Attach(IntPtr ptr) : base(ptr) { }
        }
    }
}
