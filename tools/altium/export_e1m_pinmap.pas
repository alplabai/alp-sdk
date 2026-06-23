{ ==========================================================================
  export_e1m_pinmap.pas  --  Altium DelphiScript

  ONE reusable exporter.  Run it on ANY board project (E1M-EVK,
  E1M-X-EVK, a SoM project, ...) to pull GROUND-TRUTH data out of the
  schematics so Alp SDK board metadata can be DERIVED rather than
  hand-transcribed.  One run writes two CSVs next to each other:

    <base>_pinmap.csv      Net, Designator, PinNumber, PinName
    <base>_components.csv  Designator, Comment, DNP, Parameters

  WHY pin NAMES matter: a normal netlist export gives net <-> pin NUMBER
  but drops the pin NAME.  For an E1M / E1M-X module connector (e.g. E2)
  the pin name IS the E1M-X pad (IO4, PWM3, ADC0, ENC0_X, ...), so the
  pin-map yields  pad <-> net <-> on-board chip  in one file.  The
  components dump captures every parameter, so the MPN / Manufacturer
  values come straight from the schematic whatever they are named.

  DNP / ASSEMBLY VARIANTS: a board project can carry several assembly
  VARIANTS (Project -> Variants...).  Each variant overrides the base
  design with a list of component VARIATIONS; a component whose variation
  kind is "Not Fitted" is DNP (Do-Not-Populate) in that variant.  The run
  asks WHICH variant to read (the default is the first one, or the base
  "[No Variations]" design when the project has none) and stamps a DNP
  column -- 1 for Not-Fitted, else 0 -- on every component row, so the
  derived metadata knows what is actually populated on that build.

  HOW TO RUN
    1. Open the board project so it is the FOCUSED project.
    2. File -> Run Script... -> pick  ExportAll  -> Run.
    3. At the prompt, set the output base path -- RENAME IT PER BOARD
       (e.g. ...\xevk, ...\e1m_evk, ...\v2n).  Default:
         %TEMP%\board
    4. At the next prompt, pick the assembly variant to read for DNP.
    5. Send both CSVs over.

  Builds output in memory and writes with SaveToFile (no lingering file
  handle).  The pin-map is written first, so even if the component pass
  hits a DM_* member name your Altium build spells differently, the
  pin-map CSV is already safely on disk.

  API NOTE: components + nets live on the FLATTENED document, not on
  IProject.  If a DM_* member is "Undeclared identifier", paste it --
  likely per-build swaps: DM_DocumentFlattened<->DM_FlattenedDocument,
  DM_NetName<->DM_FlattenedNetName, DM_PhysicalPartDesignator<->
  DM_PartDesignator, DM_ParameterCount/DM_Parameters/IParameter spelling.
  Variant-side swaps to try the same way: DM_ProjectVariantCount/
  DM_ProjectVariants<->DM_VariantCount/DM_Variants, DM_Description<->
  DM_VariantName, DM_VariationCount/DM_Variations<->DM_GraphicalCount/...,
  DM_PhysicalDesignator<->DM_PhysicalPartDesignator on IComponentVariation,
  and the constant eVariation_NotFitted<->eVariationNotFitted.
  ========================================================================== }

{ Return the set of DNP (Not-Fitted) physical designators for the variant
  named VarName.  Empty list when VarName is blank/not found (base design,
  everything fitted).  Caller owns the result. }
Function BuildDnpSet(Prj : IProject; VarName : String) : TStringList;
Var
    v, c    : Integer;
    Variant : IProjectVariant;
    Variation : IComponentVariation;
Begin
    Result := TStringList.Create;
    If VarName = '' Then Exit;
    For v := 0 To Prj.DM_ProjectVariantCount - 1 Do
    Begin
        Variant := Prj.DM_ProjectVariants(v);
        If Variant.DM_Description <> VarName Then Continue;
        For c := 0 To Variant.DM_VariationCount - 1 Do
        Begin
            Variation := Variant.DM_Variations(c);
            If Variation.DM_VariationKind = eVariation_NotFitted Then
                Result.Add(Variation.DM_PhysicalDesignator);
        End;
    End;
End;

{ Ask which variant to read.  Lists the available names in the prompt;
  default is the first variant, or '' (base design) when there are none. }
Function PickVariant(Prj : IProject) : String;
Var
    v    : Integer;
    names, dflt : String;
Begin
    names := '';
    dflt  := '';
    For v := 0 To Prj.DM_ProjectVariantCount - 1 Do
    Begin
        names := names + '  ' + Prj.DM_ProjectVariants(v).DM_Description + #13#10;
        If v = 0 Then dflt := Prj.DM_ProjectVariants(v).DM_Description;
    End;
    If names = '' Then names := '  (none -- base design, all fitted)' + #13#10;
    Result := InputBox('ALP board export -- assembly variant',
                       'Variant to read for DNP (blank = base, all fitted):' + #13#10 + names,
                       dflt);
End;

Procedure ExportAll;
Var
    WS      : IWorkspace;
    Prj     : IProject;
    Doc     : IDocument;
    i, k    : Integer;
    Pin     : IPin;
    Comp    : IComponent;
    Param   : IParameter;
    base, params, varName, dnp, tmpDir : String;
    nPins, nComps, nDnp : Integer;
    sl, dnpSet : TStringList;
Begin
    WS := GetWorkspace;
    If WS = Nil Then Begin ShowMessage('No workspace open.'); Exit; End;

    Prj := WS.DM_FocusedProject;
    If Prj = Nil Then Begin ShowMessage('Focus a board project first.'); Exit; End;

    Prj.DM_Compile;                       { build connectivity }
    Doc := Prj.DM_DocumentFlattened;      { components + nets live here }
    If Doc = Nil Then Begin ShowMessage('No flattened document.'); Exit; End;

    { Default to the project's own folder -- it always exists and needs no
      %TEMP% expansion (DelphiScript's SaveToFile does NOT expand env vars,
      and this build has no GetEnvironmentVariable).  ExtractFilePath keeps
      the trailing backslash.  (Swap DM_ProjectFullPath<->DM_ProjectFileName
      if "Undeclared".) }
    tmpDir := ExtractFilePath(Prj.DM_ProjectFullPath);
    base := InputBox('ALP board export',
                     'Output base path (no extension) -- rename per board:',
                     tmpDir + 'board');
    If base = '' Then Exit;

    { ---- selected assembly variant -> set of DNP (Not-Fitted) designators ---- }
    varName := PickVariant(Prj);
    dnpSet  := BuildDnpSet(Prj, varName);

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
    sl.Add('Designator,Comment,DNP,Parameters');
    nDnp := 0;
    For i := 0 To Doc.DM_ComponentCount - 1 Do
    Begin
        Comp := Doc.DM_Components(i);
        params := '';
        For k := 0 To Comp.DM_ParameterCount - 1 Do
        Begin
            Param := Comp.DM_Parameters(k);
            params := params + Param.DM_Name + '=' + Param.DM_Value + ' | ';
        End;
        If dnpSet.IndexOf(Comp.DM_PhysicalDesignator) >= 0 Then
        Begin
            dnp := '1';
            Inc(nDnp);
        End
        Else
            dnp := '0';
        sl.Add('"' + Comp.DM_PhysicalDesignator + '","' + Comp.DM_Comment + '",' + dnp + ',"' + params + '"');
    End;
    nComps := sl.Count - 1;
    sl.SaveToFile(base + '_components.csv');
    sl.Free;
    dnpSet.Free;

    ShowMessage('Done -- ' + IntToStr(nPins) + ' pins, ' + IntToStr(nComps) + ' components, ' +
                IntToStr(nDnp) + ' DNP (variant: ' + varName + '):' + #13#10 +
                base + '_pinmap.csv' + #13#10 + base + '_components.csv');
End;
