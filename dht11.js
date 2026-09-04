const API_URL = "http://192.168.69.104/custom/api/dht11";
const UPDATE_INTERVAL = 3000;

async function loadDHT11() {
    try {
        const response = await fetch(API_URL);

        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const data = await response.json();

        if (!Array.isArray(data) || data.length === 0) {
            throw new Error("API 沒有資料");
        }

        const sensor = data[0];

        document.getElementById("temp").textContent =
            `${Number(sensor.temperature).toFixed(1)}°C`;

        document.getElementById("humi").textContent =
            `${Number(sensor.humidity).toFixed(1)}%`;

    } catch (error) {
        console.error("DHT11 讀取失敗:", error);

        document.getElementById("temp").textContent = "--°C";
        document.getElementById("humi").textContent = "--%";
    }
}

document.addEventListener("DOMContentLoaded", () => {
    loadDHT11();
    setInterval(loadDHT11, UPDATE_INTERVAL);
});