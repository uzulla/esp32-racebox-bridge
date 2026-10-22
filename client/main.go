// racebox-dump receives the JSON datagrams broadcast by the ESP32 bridge and
// prints them.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"time"
)

type message struct {
	Type string `json:"type"`

	// type=racebox
	Seq           uint32                    `json:"seq"`
	Device        string                    `json:"device"`
	Time          *string                   `json:"time"`
	TimeConfirmed bool                      `json:"time_confirmed"`
	Fix           int                       `json:"fix"`
	FixOK         bool                      `json:"fix_ok"`
	Sats          int                       `json:"sats"`
	Lat           float64                   `json:"lat"`
	Lon           float64                   `json:"lon"`
	HAcc          float64                   `json:"h_acc_m"`
	SpeedMps      float64                   `json:"speed_mps"`
	Heading       float64                   `json:"heading_deg"`
	G             struct{ X, Y, Z float64 } `json:"g"`

	// type=status
	BLE            string `json:"ble"`
	DataPackets    uint32 `json:"data_packets"`
	ChecksumErrors uint32 `json:"checksum_errors"`
	DroppedBytes   uint32 `json:"dropped_bytes"`
	WifiRSSI       int    `json:"wifi_rssi"`
}

func main() {
	addr := flag.String("listen", ":5005", "UDP address to listen on")
	raw := flag.Bool("raw", false, "print the received JSON as is")
	flag.Parse()

	conn, err := net.ListenPacket("udp4", *addr)
	if err != nil {
		log.Fatal(err)
	}
	defer conn.Close()
	log.Printf("listening on %s", conn.LocalAddr())

	var lastSeq uint32
	var haveSeq bool
	buf := make([]byte, 2048)
	for {
		n, from, err := conn.ReadFrom(buf)
		if err != nil {
			log.Fatal(err)
		}
		now := time.Now().Format("15:04:05.000")
		if *raw {
			fmt.Printf("%s %s %s\n", now, from, buf[:n])
			continue
		}
		var m message
		if err := json.Unmarshal(buf[:n], &m); err != nil {
			log.Printf("%s: invalid JSON: %v: %q", from, err, buf[:n])
			continue
		}
		switch m.Type {
		case "racebox":
			if haveSeq && m.Seq != lastSeq+1 && m.Seq != 0 {
				log.Printf("lost %d datagram(s) (seq %d -> %d)", m.Seq-lastSeq-1, lastSeq, m.Seq)
			}
			lastSeq, haveSeq = m.Seq, true
			t := "-"
			if m.Time != nil {
				t = *m.Time
				if !m.TimeConfirmed {
					t += "(unconfirmed)"
				}
			}
			fmt.Printf("%s #%d %s fix=%d ok=%v sats=%d lat=%.7f lon=%.7f hacc=%.2fm speed=%.2fkm/h hdg=%.1f g=(%.2f,%.2f,%.2f)\n",
				now, m.Seq, t, m.Fix, m.FixOK, m.Sats, m.Lat, m.Lon, m.HAcc,
				m.SpeedMps*3.6, m.Heading, m.G.X, m.G.Y, m.G.Z)
		case "status":
			fmt.Printf("%s [status from %s] ble=%s device=%q data=%d cksum_err=%d dropped=%d rssi=%d\n",
				now, from, m.BLE, m.Device, m.DataPackets, m.ChecksumErrors, m.DroppedBytes, m.WifiRSSI)
		default:
			fmt.Printf("%s %s unknown: %s\n", now, from, buf[:n])
		}
	}
}
