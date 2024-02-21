function var = readjsonfile(fname)
%READJSONFILE jsonファイルからデータを読み込む
arguments
    fname {mustBeFile}
end
fid = fopen(fname);
raw = fread(fid,inf);
str = char(raw');
fclose(fid);
var = jsondecode(str);
end

