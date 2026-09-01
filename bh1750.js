const BH1750_API_URL = "/custom/api/bh1750";
const BH1750_UPDATE_INTERVAL = 3000;

console.log("bh1750.js loaded");

async function loadBH1750() {
    try {
        const response = await fetch(BH1750_API_URL);

        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const data = await response.json();

        if (!Array.isArray(data) || data.length === 0) {
            throw new Error("API 沒有資料");
        }

        const sensor = data[0];

        document.getElementById("lux").textContent =
            `${Number(sensor.lux).toFixed(1)} lx`;

    } catch (error) {
        console.error("BH1750 讀取失敗:", error);

        document.getElementById("lux").textContent = "-- lx";
    }
}

document.addEventListener("DOMContentLoaded", () => {
    loadBH1750();
    setInterval(loadBH1750, BH1750_UPDATE_INTERVAL);
});