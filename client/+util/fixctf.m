function fixedCtf = fixctf(originalCtf,ratio)
arguments
    originalCtf
    ratio = 0.5;
end
len = length(originalCtf);
ctfPhase = (angle(originalCtf(2,:)) + angle(originalCtf(end,:)))/2;
ctfDcAmp   = (abs(originalCtf(2,:)) + abs(originalCtf(end,:)))/2;
originalCtf(1,:)   = ctfDcAmp.*exp(1j*ctfPhase);
fixedCtf = fftshift(originalCtf);
fixedRange = len*(1-ratio)/2+1:len*(1+ratio)/2;
fixedCtf = fftshift(fixedCtf(fixedRange,:));
end

