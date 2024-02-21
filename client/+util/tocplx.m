function complexVal = tocplx(data)
complexVal = data(1:2:end) + 1j * data(2:2:end);
end