function complexVal = readcplxfile(filename)
arguments
    filename {mustBeFile}
end
fid = fopen (filename,'rb');
if (fid < 0)
    warning("Warning: The specified file does not exist.")
    complexVal = 0;
else
    val = fread(fid,Inf,'float');
    complexVal = val(1:2:end) + 1j * val(2:2:end);
end
if(fid ~= -1)
    fclose(fid);
end
end