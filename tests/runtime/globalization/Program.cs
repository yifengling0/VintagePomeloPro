using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

var result = new StringBuilder();
int failures = 0;
File.WriteAllText(@"C:\vp-culture-result.txt", "probe started\n");
void Check(Func<bool> evaluate, string label) {
    bool ok;
    try { ok = evaluate(); }
    catch (Exception exception) {
        result.AppendLine(exception.ToString());
        ok = false;
    }
    result.AppendLine((ok ? "PASS " : "FAIL ") + label); if (!ok) failures++;
    File.WriteAllText(@"C:\vp-culture-result.txt", result.ToString());
}
try {
    Check(() => string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DOTNET_SYSTEM_GLOBALIZATION_USENLS")), "no NLS override");
    Check(() => string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT")), "no invariant override");
    Check(() => "i".ToUpper(new CultureInfo("tr-TR")) == "\u0130", "Turkish dotted I");
    Check(() => "\u00e9".Normalize(NormalizationForm.FormD) == "e\u0301", "Unicode normalization");
    Check(() => new CultureInfo("de-DE").CompareInfo.Compare("stra\u00dfe", "STRASSE", CompareOptions.IgnoreCase) != 0, "culture comparison");
    Check(() => DateTime.Parse("31.12.2024", new CultureInfo("de-DE")).Day == 31, "localized date parsing");
    Check(() => decimal.Parse("1.234,50", new CultureInfo("de-DE")) == 1234.5m, "localized decimal parsing");
    Check(() => new CultureInfo("zh-CN").DateTimeFormat.GetMonthName(1).Length > 0, "Chinese culture data");
    Check(() => CultureInfo.GetCultures(CultureTypes.SpecificCultures).Length > 100, "full culture inventory");
    Check(() => CharUnicodeInfo.GetUnicodeCategory("\U0001f600",0) == UnicodeCategory.OtherSymbol, "supplementary Unicode character");
    result.AppendLine("ICU sort version=" + CultureInfo.GetCultureInfo("en-US").CompareInfo.Version.FullVersion);
    result.AppendLine("runtime=" + RuntimeInformation.FrameworkDescription);
} catch (Exception exception) { failures++; result.AppendLine(exception.ToString()); }
result.AppendLine("failures=" + failures);
File.WriteAllText(@"C:\vp-culture-result.txt", result.ToString());
return failures;
