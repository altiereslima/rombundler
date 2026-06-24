$bytes = [System.IO.File]::ReadAllBytes("c:\Projetos\rombundler\cabin.ttf")
$hex = foreach ($b in $bytes) { "0x{0:x2}" -f $b }
$content = "const unsigned char cabin_font_data[] = {`n" + ($hex -join ", ") + "`n};`nconst unsigned int cabin_font_data_len = $($bytes.Length);`n"
[System.IO.File]::WriteAllText("c:\Projetos\rombundler\cabin_font_data.h", $content)
