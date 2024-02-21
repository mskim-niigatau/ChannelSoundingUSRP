% Tx/Rx 切り替えテストプログラム with マスター制御
% 2023/09/26

clear
close all

Logger = util.LoggerClass;
conf = util.readjsonfile(fullfile("config","slave_config.json"));

MASTER_TCP_PORT = conf.MASTER_TCP_PORT;
MASTER_UDP_ADDR = conf.MASTER_UDP_ADDR;
MASTER_UDP_PORT = conf.MASTER_UDP_PORT;

CORE_ADDR = "127.0.0.1";
CORE_UDP_PORT = 12345;
CORE_TCP_PORT = 54321;

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

nMeasure = 10;

%% setup udp and tcp socket

args = sprintf("--tx-file %s --freq %.3fe9 --rate %de6 --lo_off -1 --tx-gain 25 --rx-gain 37.5" + ...
    " --rx-ant TX/RX --ref %s --samps %d --tx-ports %d --rx-ports %d --delay %d" + ...
    " --tcp-port %s --repeat", ...
    txFile, freq/1e9, rate/1e6, ref, nSampsPerOnce, nTxPort ,nRxPort, nDelayTotal, string(CORE_TCP_PORT));

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

core_udpSock = udpport("LocalHost",CORE_ADDR,"LocalPort",CORE_UDP_PORT,"Timeout",0.1);
core_udpSock.flush
core_tcpSock = tcpclient(CORE_ADDR,CORE_TCP_PORT);
core_udpSock.Timeout = 1;

master_tcpSock = tcpserver(MASTER_TCP_PORT, "Timeout", 600);    %10分間応答が無いとタイムアウト

masterUdpSock = udpport("datagram","IPV4");

%%

Logger.info("Waiting for connection from master PC.")
while(~master_tcpSock.Connected)
    pause(0.01)
end
Logger.info("Connected.")

%%
ctf = zeros(nSampsPerOnce,nRxPort,nTxPort);

if(waittcpdata(core_tcpSock) ~= '0')
    clear("core_tcpSock")
    return
end

while(1)
    while(master_tcpSock.Connected && ~logical(master_tcpSock.NumBytesAvailable))
        pause(.1)
    end
    if(~master_tcpSock.Connected)
        Logger.info("Master PC connection lost.")
        break
    else
        tcpData = master_tcpSock.read(master_tcpSock.NumBytesAvailable,"char");
    end
    
    if(tcpData(1) == '3')
        % Start Rx
        tcpStr = split(convertCharsToStrings(tcpData),'$');
        if(length(tcpStr) ~= 4)
            Logger.info("unknown command")
            break
        else
            txNo = tcpStr(2);
            saveFileName = tcpStr(3);
            shouldSendUdp = tcpStr(4);
        end
        Logger.info("Recieving data...")
        clear ytAll yt yf
        
        reply = '';
        while(true)
            core_udpSock.flush;
            core_tcpSock.write('3')
            reply = waittcpdata(core_tcpSock);
            if(reply ~= '4')
                break
            end
        end
        if(reply ~= '3')
            Logger.info("Unexpected error occured.")
            break
        end
        
        ytAll = util.tocplx(core_udpSock.read(nSampsTotal*2,"single"));
        yt = reshape(ytAll,nSampsPerOnce*2,nRxPort,nTxPort);
        yt(1:nSampsPerOnce,:,:) = [];
        yf = fft(yt);
        
        tiledlayout(3,1);
        nexttile
        plot(real(ytAll))
        title('ytAll')
        xlabel('samples')
        nexttile
        plot(real(yt(:,1,1)))
        title('yt')
        xlabel('samples')
        nexttile
        plot(fftshift(pow2db(abs(yf(:,1,1)).^2)))
        title('yf')
        ylabel('[dB]')
        xlabel('samples')
        if(strcmp(shouldSendUdp,"true"))
            udpData = util.cplx2double(reshape(yt,numel(yt),1));
            masterUdpSock.write(udpData,"double",MASTER_UDP_ADDR,MASTER_UDP_PORT)
        end
        if(saveFileName ~= "")
            save(fullfile(saveDir,saveFileName + "_" + txNo + ".mat"),"yt")
        end
        master_tcpSock.write("3")
        Logger.info("Done.")
        
        
    elseif(tcpData(1) == '1')
        % Start Tx
        Logger.info("Sending data...")
        core_udpSock.flush;
        core_tcpSock.write('1')
        if(waittcpdata(core_tcpSock) ~= '1')
            break
        end
        master_tcpSock.write("1")
        
    elseif(tcpData(1) == '2')
        % End Tx
        core_udpSock.flush;
        core_tcpSock.write('2')
        if(waittcpdata(core_tcpSock) ~= '2')
            break
        end
        master_tcpSock.write("2")
        Logger.info("Done.")
    else
        Logger.info("Unknown command.")
        break
    end
end

clear("core_tcpSock")

core_udpSock.flush
core_udpSock.delete

clear("master_tcpSock")

close all

function tcpData = waittcpdata(sock)
while(true)
    numBytesAvailable = sock.NumBytesAvailable;
    if(logical(numBytesAvailable))
        tcpData = sock.read(numBytesAvailable,"char");
        break
    end
end
end

