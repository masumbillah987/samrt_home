const express = require("express");
const app = express();

let deviceState = {
  fan: 0,
  light1: 0,
  light2: 0
};

app.get("/device/status", (req,res)=>{
  res.json(deviceState);
});

app.get("/fan/on",(req,res)=>{
  deviceState.fan = 1;
  res.send("Fan ON");
});

app.get("/fan/off",(req,res)=>{
  deviceState.fan = 0;
  res.send("Fan OFF");
});

app.get("/light1/on",(req,res)=>{
  deviceState.light1 = 1;
  res.send("Light1 ON");
});

app.get("/light1/off",(req,res)=>{
  deviceState.light1 = 0;
  res.send("Light1 OFF");
});

app.listen(3000, ()=>{
  console.log("Server running");
});
