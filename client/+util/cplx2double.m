function doubleVal = cplx2double(data)
dataSize = size(data);
dataSize(1) = dataSize(1)*2;
doubleVal = zeros(dataSize);
doubleVal(1:2:end,:) = real(data);
doubleVal(2:2:end,:) = real(data);
end