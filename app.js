// ==========================================
// TAHAP 2: Firebase Initialization & Logic
// ==========================================

// 1. Initialize Firebase using a placeholder config object.
const firebaseConfig = {
    apiKey: "AIzaSyDNBwjTrdDQh2prE2olTlP1hmQ8779uszo",
    authDomain: "ats-system-9640b.firebaseapp.com",
    databaseURL: "https://ats-system-9640b-default-rtdb.firebaseio.com",
    projectId: "ats-system-9640b",
    storageBucket: "ats-system-9640b.firebasestorage.app",
    messagingSenderId: "742975280398",
    appId: "1:742975280398:web:ece278220fafe180e6b35f",
    measurementId: "G-Y4XFY1Q180"
};

// Initialize Firebase only if not already initialized
if (!firebase.apps.length) {
    firebase.initializeApp(firebaseConfig);
}
const db = firebase.database();

// --- DOM References ---

// Top Banner
const elStatusPengisianBanner = document.getElementById('val-status-pengisian-banner');
const elBannerDaya = document.getElementById('val-banner-daya');
const elBadgeStatus = document.getElementById('badge-status');

// Card 1: Status Sistem
const elStatusPln = document.getElementById('val-status-pln');
const elStatusInverter = document.getElementById('val-status-inverter');
const elStatusPengisian = document.getElementById('val-status-pengisian');
const elModeSistem = document.getElementById('val-mode-sistem');

// Card 2: Parameter AC
const elTeganganAc = document.getElementById('val-tegangan-ac');
const elArusAc = document.getElementById('val-arus-ac');
const elDayaAc = document.getElementById('val-daya-ac');
const elEnergiAc = document.getElementById('val-energi-ac');

// Card 3: Parameter Baterai
const elTeganganBaterai = document.getElementById('val-tegangan-baterai');
const elArusBeban = document.getElementById('val-arus-beban');
const elSocBar = document.getElementById('soc-bar');
const elValPersentase = document.getElementById('val-persentase');
const elStatusBeban = document.getElementById('val-status-beban');


// --- Helper Functions ---
const formatNum = (val, decimals = 1) => val !== undefined && val !== null ? Number(val).toFixed(decimals) : '--';
const formatStr = (val) => val || '--';


// 2. Real-time Listener for Middle Panel & Banner
const realtimeRef = db.ref('/SmartATS/Realtime');

realtimeRef.on('value', (snapshot) => {
    const data = snapshot.val();
    if (!data) return; // Handle gracefully if no data

    // --- Update Card 1: Status Sistem ---
    elStatusPln.textContent = formatStr(data.status_pln);
    elStatusInverter.textContent = formatStr(data.status_inverter);
    elStatusPengisian.textContent = formatStr(data.status_pengisian);
    elModeSistem.textContent = formatStr(data.mode_sistem);

    // --- Update Card 2: Parameter AC ---
    elTeganganAc.textContent = `${formatNum(data.tegangan_ac, 1)} V`;
    elArusAc.textContent = `${formatNum(data.arus_ac, 2)} A`;
    
    const dayaAcRaw = data.daya_ac !== undefined && data.daya_ac !== null ? Number(data.daya_ac) : null;
    elDayaAc.textContent = dayaAcRaw !== null ? `${dayaAcRaw.toFixed(1)} W` : '-- W';
    
    elEnergiAc.textContent = `${formatNum(data.energi_ac, 2)} kWh`;

    // --- Update Card 3: Parameter Baterai ---
    elTeganganBaterai.textContent = `${formatNum(data.tegangan_baterai, 1)} V`;
    elArusBeban.textContent = `${formatNum(data.arus_beban_baterai, 2)} A`;
    elStatusBeban.textContent = formatStr(data.status_beban);

    // Battery SOC Progress Bar
    const soc = data.persentase_baterai !== undefined && data.persentase_baterai !== null ? Number(data.persentase_baterai) : 0;
    elSocBar.style.width = `${soc}%`;
    elValPersentase.textContent = `${soc.toFixed(0)}%`;
    
    // Dynamic color for Battery SOC
    if (soc <= 20) {
        elSocBar.style.background = 'linear-gradient(90deg, var(--danger), #f56565)';
    } else if (soc <= 50) {
        elSocBar.style.background = 'linear-gradient(90deg, var(--warning), #f6ad55)';
    } else {
        elSocBar.style.background = 'linear-gradient(90deg, var(--success), #48bb78)';
    }

    // --- Update Top Banner ---
    elStatusPengisianBanner.textContent = formatStr(data.status_pengisian);
    elBannerDaya.textContent = dayaAcRaw !== null ? `${dayaAcRaw.toFixed(1)} W` : '-- W';

    // Banner Badge Logic
    if (dayaAcRaw !== null && dayaAcRaw > 450) {
        elBadgeStatus.textContent = "OVERLOAD";
        elBadgeStatus.className = "badge overload";
    } else {
        elBadgeStatus.textContent = "AMAN";
        elBadgeStatus.className = "badge aman";
    }
});


// ==========================================
// TAHAP 3: History Feature Logic
// ==========================================

const elCurrentDate = document.getElementById('current-date');
const elHistoryContainer = document.getElementById('history-container');

// 1. Get current date in 'YYYY-MM-DD' format
function getCurrentDateFormatted() {
    const d = new Date();
    const year = d.getFullYear();
    const month = String(d.getMonth() + 1).padStart(2, '0');
    const day = String(d.getDate()).padStart(2, '0');
    return `${year}-${month}-${day}`;
}

const currentDate = getCurrentDateFormatted();
elCurrentDate.textContent = currentDate; // Display date in heading

// 2. Create Firebase listener for history path
const historyRef = db.ref(`/SmartATS/History/${currentDate}`);

historyRef.on('value', (snapshot) => {
    const historyData = snapshot.val();
    
    // 3. Clear container
    elHistoryContainer.innerHTML = '';

    if (!historyData) {
        elHistoryContainer.innerHTML = '<div class="placeholder-text">Belum ada data riwayat untuk hari ini.</div>';
        return;
    }

    // 4. Sort keys so the newest hour is at the TOP
    const hours = Object.keys(historyData).sort((a, b) => b.localeCompare(a));

    // 5 & 6. Dynamically generate HTML rows for each hour
    hours.forEach(hour => {
        const data = historyData[hour];
        
        const mode = data.mode_sistem || '--';
        const dayaAcRaw = data.daya_ac !== undefined && data.daya_ac !== null ? Number(data.daya_ac) : 0;
        const socRaw = data.persentase_baterai !== undefined && data.persentase_baterai !== null ? Number(data.persentase_baterai) : 0;
        
        // Red text warning if daya > 450
        const isOverload = dayaAcRaw > 450;
        const powerClass = isOverload ? 'history-power warning' : 'history-power';

        const row = document.createElement('div');
        row.className = 'history-row';
        row.innerHTML = `
            <div class="history-hour">${hour}</div>
            <div class="history-mode">Mode: <span>${mode}</span></div>
            <div class="${powerClass}">Daya: ${dayaAcRaw.toFixed(1)} W</div>
            <div class="history-soc">Baterai: ${socRaw.toFixed(0)}%</div>
        `;
        
        elHistoryContainer.appendChild(row);
    });
});
