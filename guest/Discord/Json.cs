using System;
using System.Collections.Generic;
using System.Text;

// Enough JSON for Discord's REST replies. Numbers stay text so snowflake ids are not rounded.
public class Json
{
    public enum Kind { Null, Bool, Number, String, Array, Object }

    public Kind kind;
    public bool b;
    public string s = "";
    public List<Json> arr;
    public Dictionary<string, Json> obj;

    static readonly Json Missing = new Json();
    static readonly List<Json> NoItems = new List<Json>();

    public static Json Parse(string text)
    {
        Parser p = new Parser(text);
        Json v = p.ParseValue();
        p.Skip();
        if (p.i != p.n) throw new FormatException("Trailing data");
        return v;
    }

    public Json Get(string key)
    {
        Json v;
        if (obj != null && obj.TryGetValue(key, out v) && v != null) return v;
        return Missing;
    }

    public List<Json> Items { get { return arr != null ? arr : NoItems; } }

    public string Str(string key)
    {
        Json v = Get(key);
        if (v.kind == Kind.String || v.kind == Kind.Number) return v.s != null ? v.s : "";
        if (v.kind == Kind.Bool) return v.b ? "true" : "false";
        return "";
    }

    public int Int(string key, int fallback)
    {
        string raw = Str(key);
        if (raw.Length == 0) return fallback;
        int n = 0, i = 0, sign = 1;
        if (raw[0] == '-') { sign = -1; i++; }
        if (i >= raw.Length || raw[i] < '0' || raw[i] > '9') return fallback;
        for (; i < raw.Length; i++)
        {
            if (raw[i] < '0' || raw[i] > '9') break;
            n = n * 10 + (raw[i] - '0');
        }
        return sign < 0 ? -n : n;
    }

    public static string Quote(string value)
    {
        StringBuilder b = new StringBuilder();
        b.Append('"');
        if (value != null)
        {
            for (int i = 0; i < value.Length; i++)
            {
                char c = value[i];
                switch (c)
                {
                    case '"':
                    case '\\': b.Append('\\'); b.Append(c); break;
                    case '\n': b.Append("\\n"); break;
                    case '\r': b.Append("\\r"); break;
                    case '\t': b.Append("\\t"); break;
                    default:
                        if (c < 32) b.Append("\\u").Append(((int)c).ToString("x4"));
                        else b.Append(c);
                        break;
                }
            }
        }
        b.Append('"');
        return b.ToString();
    }

    sealed class Parser
    {
        readonly string t;
        public int i, n;

        public Parser(string text)
        {
            t = text ?? "";
            n = t.Length;
            if (n > 0 && t[0] == '\uFEFF') i = 1;
        }

        public void Skip()
        {
            while (i < n)
            {
                char c = t[i];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
                i++;
            }
        }

        public Json ParseValue()
        {
            Skip();
            if (i >= n) throw new FormatException("Unexpected end");
            char c = t[i];
            if (c == '{') return ParseObject();
            if (c == '[') return ParseArray();
            if (c == '"') { Json s = new Json(); s.kind = Kind.String; s.s = ReadString(); return s; }
            if (c == 't' || c == 'f') return ParseBool();
            if (c == 'n') return ParseNull();
            if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
            throw new FormatException("Bad value");
        }

        Json ParseObject()
        {
            i++;
            Json o = new Json();
            o.kind = Kind.Object;
            o.obj = new Dictionary<string, Json>();
            Skip();
            if (i < n && t[i] == '}') { i++; return o; }
            while (true)
            {
                Skip();
                if (i >= n || t[i] != '"') throw new FormatException("Expected key");
                string key = ReadString();
                Skip();
                if (i >= n || t[i] != ':') throw new FormatException("Expected colon");
                i++;
                o.obj[key] = ParseValue();
                Skip();
                if (i >= n) throw new FormatException("Unclosed object");
                if (t[i] == '}') { i++; break; }
                if (t[i] != ',') throw new FormatException("Expected comma");
                i++;
            }
            return o;
        }

        Json ParseArray()
        {
            i++;
            Json a = new Json();
            a.kind = Kind.Array;
            a.arr = new List<Json>();
            Skip();
            if (i < n && t[i] == ']') { i++; return a; }
            while (true)
            {
                a.arr.Add(ParseValue());
                Skip();
                if (i >= n) throw new FormatException("Unclosed array");
                if (t[i] == ']') { i++; break; }
                if (t[i] != ',') throw new FormatException("Expected comma");
                i++;
            }
            return a;
        }

        Json ParseBool()
        {
            if (Match("true")) { Json v = new Json(); v.kind = Kind.Bool; v.b = true; return v; }
            if (Match("false")) { Json v = new Json(); v.kind = Kind.Bool; return v; }
            throw new FormatException("Bad bool");
        }

        Json ParseNull()
        {
            if (!Match("null")) throw new FormatException("Bad null");
            return new Json();
        }

        Json ParseNumber()
        {
            int start = i;
            if (t[i] == '-') i++;
            while (i < n)
            {
                char c = t[i];
                if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') i++;
                else break;
            }
            if (i == start || (i == start + 1 && t[start] == '-')) throw new FormatException("Bad number");
            Json v = new Json();
            v.kind = Kind.Number;
            v.s = t.Substring(start, i - start);
            return v;
        }

        string ReadString()
        {
            if (i >= n || t[i] != '"') throw new FormatException("Expected string");
            i++;
            StringBuilder b = new StringBuilder();
            while (i < n)
            {
                char c = t[i];
                if (c == '"') { i++; return b.ToString(); }
                if (c == '\\')
                {
                    i++;
                    if (i >= n) throw new FormatException("Bad escape");
                    char e = t[i];
                    if (e == '"' || e == '\\' || e == '/') b.Append(e);
                    else if (e == 'b') b.Append('\b');
                    else if (e == 'f') b.Append('\f');
                    else if (e == 'n') b.Append('\n');
                    else if (e == 'r') b.Append('\r');
                    else if (e == 't') b.Append('\t');
                    else if (e == 'u')
                    {
                        int code = 0;
                        for (int k = 0; k < 4; k++)
                        {
                            i++;
                            if (i >= n) throw new FormatException("Bad escape");
                            int d = Hex(t[i]);
                            if (d < 0) throw new FormatException("Bad escape");
                            code = (code << 4) + d;
                        }
                        b.Append((char)code);
                    }
                    else throw new FormatException("Bad escape");
                }
                else b.Append(c);
                i++;
            }
            throw new FormatException("Unclosed string");
        }

        bool Match(string word)
        {
            if (i + word.Length > n) return false;
            for (int k = 0; k < word.Length; k++)
                if (t[i + k] != word[k]) return false;
            i += word.Length;
            return true;
        }

        static int Hex(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }
    }
}
