clear
close all

Logger = util.LoggerClass;
conf = util.readjsonfile(fullfile("config","slave_config.json"));

MASTER_TCP_PORT = conf.MASTER_TCP_PORT;
MASTER_UDP_ADDR = conf.MASTER_UDP_ADDR;
MASTER_UDP_PORT = conf.MASTER_UDP_PORT;

ADDR = "127.0.0.1";
UDP_PORT = 12345;
TCP_PORT = 54321;

saveDir = fullfile("recieve_data",conf.saveDir);
if(~logical(exist(saveDir,"dir")))
    mkdir(saveDir)
end

freq = 4.85e9;
rate = 200e6;
ref = conf.ref;%external, internal, gpsdo

txFile = fullfile(pwd,"data","multitone_x7.dat");
execPath = fullfile(pwd,"..","txrx_core","build","txrx_core");

nSampsPerOnce = 256;
nTxPort = 8;
nRxPort = 8;
nSampsTotal = nSampsPerOnce * nTxPort * nRxPort * 2;
nDelayTotal = 256*8*8*2;    %txrx_coreの仕様上，送信信号と同期を取るためnSampsPerOnce(=256)の倍数である必要があります

nMean = 10;

%% setup udp and tcp socket
args = sprintf("--tx-file %s --freq %.3fe9 --rate %de6 --lo_off -1 --tx-gain 25 --rx-gain 37.5" + ...
    " --rx-ant TX/RX --ref %s --samps %d --tx-ports %d --rx-ports %d --delay %d" + ...
    " --tcp-port %s --repeat", ...
    txFile, freq/1e9, rate/1e6, ref, nSampsPerOnce, nTxPort ,nRxPort, nDelayTotal, string(TCP_PORT));

if isunix
    clipboard('copy', execPath + " " + args + " && exit")
    system("gnome-terminal");
    answer = questdlg('To continue, open a terminal, paste the contents of the clipboard and run it.', ...
        'Waiting for...', ...
        'Done','Cancel','Done');
    if ~isequal(answer,'Done')
        return
    end
else
    system(execPath + ".exe " + args + " & exit &");
    pause(20)
end

Logger.info("Attempt to connect the UDP socket...")
udpSock = udpport("LocalHost",ADDR,"LocalPort",UDP_PORT,"Timeout",0.1);
Logger.info("Successfully connected UDP socket!")
udpSock.flush
Logger.info("Attempt to connect the TCP socket...")
tcpSock = tcpclient(ADDR,TCP_PORT);
Logger.info("Successfully connected TCP socket!")

%% data acquisition
s = util.readcplxfile("data/multitone.dat");
sf = fft(s);
udpSock.Timeout = 1;
ctf = zeros(nSampsPerOnce,8,8);

tcpData = '';
while(isempty(tcpData))
    tcpData = char(tcpSock.read());
    pause(.01)
end
if(tcpData(1) ~= '0')
    Logger.info("tcpData = "+tcpData)
    clear("tcpSock")
    return
end

Logger.info("Start data acquisition")
yt = zeros(nSampsPerOnce,8,8);
for iTx = 1:8
    Logger.info("Tx Port: " + iTx)
    iData = 0;
    while(iData < nMean)
        udpSock.flush;
        tcpSock.write("3")
        tcpData = '';
        while(isempty(tcpData))
            tcpData = char(tcpSock.read());
            pause(.01)

        end
        if(tcpData(1) == '4')
            continue
        elseif (tcpData(1) ~= '3')
            break
        end

        data = udpSock.read(nSampsTotal*2,"single");
        data = util.tocplx(data);
        ytAll = data;
        ytTmp = reshape(ytAll,nSampsPerOnce*2,8,8);
        ytTmp(1:nSampsPerOnce,:,:) = [];
        yt(:,:,iTx) = yt(:,:,iTx) + ytTmp(:,:,iTx);

        % plot tmp data
        tiledlayout(4,2)
        for jTx = 1:8
            nexttile
            plot(real(ytTmp(:,:,jTx)))
            title("Tx: " + jTx)
        end

        iData = iData+1;
    end
end
Logger.info("Done!")
clear("tcpSock")

udpSock.flush
udpSock.delete

for iTx = 1:8
    yt(:,:,iTx) = yt(:,:,iTx)./nMean;
end
yf = fft(yt);
ctf = yf./sf;

%% save data
[FILE_NAME, FILE_DIR] = uiputfile("*.mat");
if FILE_NAME
    save(fullfile(FILE_DIR,FILE_NAME),"ctf")
end
close