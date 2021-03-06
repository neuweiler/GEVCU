var dashboard = dashboard || {};

(function() {
	var webSocketWorker;

	dashboard.activate = activate;
	dashboard.hideStateDivs = hideStateDivs;
	dashboard.sendMsg = sendMsg;
	dashboard.addChargeOptions = addChargeOptions;
	dashboard.setCruiseData = setCruiseData;

	function activate() {
		// add an event listener so sounds can get loaded on mobile devices
		// after user interaction
		window.addEventListener('keydown', removeBehaviorsRestrictions);
		window.addEventListener('mousedown', removeBehaviorsRestrictions);
		window.addEventListener('touchstart', removeBehaviorsRestrictions);
		startWebSocketWorker();
	}

	// on most mobile browsers sounds can only be loaded during a user
	// interaction (stupid !)
	function removeBehaviorsRestrictions() {
		soundError.load();
		soundWarn.load();
		soundInfo.load();
		window.removeEventListener('keydown', removeBehaviorsRestrictions);
		window.removeEventListener('mousedown', removeBehaviorsRestrictions);
		window.removeEventListener('touchstart', removeBehaviorsRestrictions);
	}

	function startWebSocketWorker() {
		if (typeof (webSocketWorker) == "undefined") {
			webSocketWorker = new Worker("worker/webSocket.js");
			webSocketWorker.onmessage = function(event) {
				processWebsocketMessage(event.data)
			};
		}
		webSocketWorker.postMessage({
			cmd : 'start'
		});
	}

	function stopWebSocketWorker() {
		if (webSocketWorker) {
			webSocketWorker.postMessage({
				cmd : 'stop'
			});
			webSocketWorker = undefined;
		}
	}

	function processWebsocketMessage(message) {
		for (name in message) {
			var data = message[name];

			var dial = Gauge.dials[name];
			if (dial) {
				dial.setValue(data);
			} else if (name == 'limits') {
				for (limitName in data) {
					var limit = data[limitName];
					dial = Gauge.dials[limitName];
					if (dial) {
						dial.setLimits(limit.min, limit.max);
					}
				}
			} else if (name.indexOf('bitfield') != -1) { // a bitfield value for annunciator fields
				updateAnnunciatorFields(name, data);
			} else if (name == 'systemState') {
				updateSystemState(data);
			} else if (name == 'logMessage') {
				log(data.level, data.message);
			} else {
				// set the meter value (additionally to a text node)
				var target = getCache(name + "Meter");
				if (target) {
					target.value = data;
				}
				setNodeValue(name, data);
				if (name == 'enableCruiseControl') {
					showHideCruiseControl(data);
				}
			}
		}
	}

	function showHideCruiseControl(data) {
		var div = document.getElementById('cruiseControl');
		div.style.display = (data ? '' : 'none');
	}
	
	function hideStateDivs() {
		var div = document.getElementsByTagName('div')
		for (i = 0; i < div.length; i++) {
			var idStr = div[i].id;
			if (idStr && idStr.indexOf('state_') != -1) {
				div[i].style.display = (idStr.indexOf('_0_') == -1 ? 'none' : '');
			}
		}
	}
	
	function log(level, message) {
		if (level == 'ERROR') {
			alertify.error(message, 0);
			soundError.play();
		} else if (level == 'WARNING') {
			alertify.warning(message, 60);
			soundWarn.play();
		} else {
			alertify.success(message, 30);
			soundInfo.play();
		}
	}

	function updateSystemState(state) {
		var div = document.getElementsByTagName('div')
		for (i = 0; i < div.length; i++) {
			var idStr = div[i].id;
			if (idStr && idStr.indexOf('state_') != -1) {
				if (idStr.indexOf('_' + state + '_') != -1) {
					div[i].style.display = '';
				} else {
					div[i].style.display = 'none';
				}
			}
		}
	}

	// send message to GEVCU via websocket
	function sendMsg(message) {
		webSocketWorker.postMessage({cmd: 'message', message: message});
	}
	
	function addChargeOptions(data) {
		var select = document.getElementById("chargeInputLevel");
		for (var level in data.chargeInputLevels) {
			select.add(new Option(data.chargeInputLevels[level], data.chargeInputLevels[level]));
		}
		select.value=data.chargeInputLevels[data.chargeInputLevels.length - 1];
	}
	
	function setCruiseData(data) {
		var spans = document.getElementsByClassName("cruiseSpeedUnit");
		for (var i = 0; i < spans.length; i++) {
		  spans[i].innerHTML = (data.cruiseUseRpm ? "rpm" : "kmh");
		}

		addCruiseButton('+' + data.cruiseSpeedStep, data.cruiseUseRpm);
		addCruiseButton('-' + data.cruiseSpeedStep, data.cruiseUseRpm);

		for (var speed in data.cruiseSpeedSet) {
			addCruiseButton(data.cruiseSpeedSet[speed], data.cruiseUseRpm);
		}
	}
	
	function addCruiseButton(speed, unit) {
		var cruiseControlDiv = document.getElementById("cruiseControl");
		
		var button = document.createElement("BUTTON");
		button.innerHTML = speed;// + " " + (unit ? "rpm" : "kmh");
		var cl = document.createAttribute("class");
		cl.value = "button";
		button.setAttributeNode(cl);  
		var oncl = document.createAttribute("onclick");
		oncl.value = "dashboard.sendMsg('cruise=" + speed +"')";
		button.setAttributeNode(oncl);  
		cruiseControlDiv.appendChild(button);
	}
})();
