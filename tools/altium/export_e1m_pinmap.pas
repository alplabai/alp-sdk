{ ==========================================================================
  export_e1m_pinmap.pas  --  Altium DelphiScript

  ONE reusable exporter.  Run it on ANY board project (E1M-EVK,
  E1M-X-EVK, a SoM project, ...) to pull GROUND-TRUTH data out of the
  schematics so ALP SDK board metadata can be DERIVED rather than
  hand-transcribed.  One run writes two CSVs next to each other:

    <base>_pinmap.csv      Net, Designator, PinNumber, PinName
    <base>_components.csv  Designator, Comment, Parameters

  WHY pin NAMES matter: a normal netlist export gives net <-> pin NUMBER
  but drops the pin NAME.  For an E1M / E1M-X module connector (e.g. E2)
  the pin name IS the E1M-X pad (IO4, PWM3, ADC0, ENC0_X, ...), so the
  pin-map yields  pad <-> net <-> on-board chip  in one file.  The
  components dump captures every parameter, so the MPN / Manufacturer
  values come straight from the schematic whatever they are named.

  HOW TO RUN
    1. Open the board project so it is the FOCUSED project.
    2. File -> Run Script... -> pick  ExportAll  -> Run.
    3. At the prompt, set the output base path -- RENAME IT PER BOARD
       (e.g. ...\xevk, ...\e1m_evk, ...\v2n).  Default:
         C:\Users\caner\AppData\Local\Temp\board
    4. Send both CSVs over.

  Builds output in memory and writes with SaveToFile (no lingering file
  handle).  The pin-map is written first, so even if the component pass
  hits a DM_* member name your Altium build spells differently, the
  pin-map CSV is already safely on disk.

  API NOTE: components + nets live on the FLATTENED document, not on
  IProject.  If a DM_* member is "Undeclared identifier", paste it --
  likely per-build swaps: DM_DocumentFlattened<->DM_FlattenedDocument,
  DM_NetName<->DM_FlattenedNetName, DM_PhysicalPartDesignator<->
  DM_PartDesignator, DM_ParameterCount/DM_Parameters/IParameter spelling.
  ========================================================================== }

Procedure ExportAll;
Var
    WS      : IWorkspace;
    Prj     : IProject;
    Doc     : IDocument;
    i, k    : Integer;
    Pin     : IPin;
    Comp    : IComponent;
    Param   : IParameter;
    base, params  : String;
    nPins, nComps : Integer;
    sl      : TStringList;
Begin
    WS := GetWorkspace;
    If WS = Nil Then Begin ShowMessage('No workspace open.'); Exit; End;

    Prj := WS.DM_FocusedProject;
    If Prj = Nil Then Begin ShowMessage('Focus a board project first.'); Exit; End;

    Prj.DM_Compile;                       { build connectivity }
    Doc := Prj.DM_DocumentFlattened;      { components + nets live here }
    If Doc = Nil Then Begin ShowMessage('No flattened document.'); Exit; End;

    base := InputBox('ALP board export',
                     'Output base path (no extension) -- rename per board:',
                     'C:\Users\caner\AppData\Local\Temp\board');
    If base = '' Then Exit;

    { ---- 1) pin map : iterate COMPONENTS -> PINS so EVERY pin is listed, ---- }
    { ---- including UNCONNECTED ones (blank Net), e.g. spare bus pads.    ---- }
    sl := TStringList.Create;
    sl.Add('Net,Designator,PinNumber,PinName');
    For i := 0 To Doc.DM_ComponentCount - 1 Do
    Begin
        Comp := Doc.DM_Components(i);
        For k := 0 To Comp.DM_PinCount - 1 Do
        Begin
            Pin := Comp.DM_Pins(k);
            sl.Add('"' + Pin.DM_FlattenedNetName + '",' +
                   Comp.DM_PhysicalDesignator + ',' +
                   Pin.DM_PinNumber + ',"' + Pin.DM_PinName + '"');
        End;
    End;
    nPins := sl.Count - 1;
    sl.SaveToFile(base + '_pinmap.csv');
    sl.Free;

    { ---- 2) components : designator, comment, ALL parameters (MPN etc.) ---- }
    sl := TStringList.Create;
    sl.Add('Designator,Comment,Parameters');
    For i := 0 To Doc.DM_ComponentCount - 1 Do
    Begin
        Comp := Doc.DM_Components(i);
        params := '';
        For k := 0 To Comp.DM_ParameterCount - 1 Do
        Begin
            Param := Comp.DM_Parameters(k);
            params := params + Param.DM_Name + '=' + Param.DM_Value + ' | ';
        End;
        sl.Add('"' + Comp.DM_PhysicalDesignator + '","' + Comp.DM_Comment + '","' + params + '"');
    End;
    nComps := sl.Count - 1;
    sl.SaveToFile(base + '_components.csv');
    sl.Free;

    ShowMessage('Done -- ' + IntToStr(nPins) + ' pins, ' + IntToStr(nComps) + ' components:' + #13#10 +
                base + '_pinmap.csv' + #13#10 + base + '_components.csv');
End;
