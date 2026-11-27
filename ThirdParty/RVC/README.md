The five Python modules in infer_pack are the RVC reference decoder distributed
with w-okada/voice-changer at commit 7e2ce5c4800010bbc7604968bd29b802534fac05
(server/voice_changer/RVC/inferencer/rvc_models/infer_pack). Its provenance file
identifies RVC-Project/Retrieval-based-Voice-Conversion-WebUI commit
66d470361ad07009a442e9e95a0169ba1a139459. Both MIT notices are included.
The build packages the decoder source. The voice checkpoint, optional FAISS index,
ContentVec and RMVPE weights are supplied locally by the user.
