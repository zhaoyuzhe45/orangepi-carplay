using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace CarPlayBox
{
    public static class CredentialStore
    {
        private const uint CredentialTypeGeneric = 1;
        private const uint PersistLocalMachine = 2;
        private const int ErrorNotFound = 1168;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NativeCredential
        {
            public uint Flags;
            public uint Type;
            public IntPtr TargetName;
            public IntPtr Comment;
            public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
            public uint CredentialBlobSize;
            public IntPtr CredentialBlob;
            public uint Persist;
            public uint AttributeCount;
            public IntPtr Attributes;
            public IntPtr TargetAlias;
            public IntPtr UserName;
        }

        [DllImport("advapi32.dll", EntryPoint = "CredReadW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CredRead(string target, uint type, uint flags, out IntPtr credential);

        [DllImport("advapi32.dll", EntryPoint = "CredWriteW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CredWrite(ref NativeCredential credential, uint flags);

        [DllImport("advapi32.dll", EntryPoint = "CredFree")]
        private static extern void CredFree(IntPtr buffer);

        public static bool Exists(string target)
        {
            IntPtr pointer;
            if (!CredRead(target, CredentialTypeGeneric, 0, out pointer))
                return false;
            CredFree(pointer);
            return true;
        }

        public static string Read(string target)
        {
            IntPtr pointer;
            if (!CredRead(target, CredentialTypeGeneric, 0, out pointer))
            {
                int error = Marshal.GetLastWin32Error();
                if (error == ErrorNotFound)
                    return null;
                throw new Win32Exception(error);
            }

            try
            {
                NativeCredential credential = (NativeCredential)Marshal.PtrToStructure(pointer, typeof(NativeCredential));
                if (credential.CredentialBlob == IntPtr.Zero || credential.CredentialBlobSize == 0)
                    return string.Empty;
                byte[] bytes = new byte[credential.CredentialBlobSize];
                Marshal.Copy(credential.CredentialBlob, bytes, 0, bytes.Length);
                return Encoding.Unicode.GetString(bytes).TrimEnd('\0');
            }
            finally
            {
                CredFree(pointer);
            }
        }

        public static void Write(string target, string userName, string password)
        {
            if (String.IsNullOrEmpty(target) || String.IsNullOrEmpty(userName) || String.IsNullOrEmpty(password))
                throw new ArgumentException("Credential target, user name, and password are required.");

            byte[] secret = Encoding.Unicode.GetBytes(password);
            IntPtr targetPointer = Marshal.StringToCoTaskMemUni(target);
            IntPtr userPointer = Marshal.StringToCoTaskMemUni(userName);
            IntPtr secretPointer = Marshal.AllocHGlobal(secret.Length);
            try
            {
                Marshal.Copy(secret, 0, secretPointer, secret.Length);
                NativeCredential credential = new NativeCredential
                {
                    Type = CredentialTypeGeneric,
                    TargetName = targetPointer,
                    CredentialBlobSize = (uint)secret.Length,
                    CredentialBlob = secretPointer,
                    Persist = PersistLocalMachine,
                    UserName = userPointer
                };
                if (!CredWrite(ref credential, 0))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            finally
            {
                byte[] zeros = new byte[secret.Length];
                Marshal.Copy(zeros, 0, secretPointer, zeros.Length);
                Marshal.FreeHGlobal(secretPointer);
                Marshal.FreeCoTaskMem(userPointer);
                Marshal.FreeCoTaskMem(targetPointer);
                Array.Clear(secret, 0, secret.Length);
            }
        }
    }
}
